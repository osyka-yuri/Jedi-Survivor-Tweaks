#pragma once

#include "memory_scanner.hpp"  // ParsedPattern

#include <array>
#include <cstdint>
#include <expected>
#include <flat_map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jst::core {

/// RAII wrapper for executable memory (trampoline buffers).
///
/// Owns/releases the memory by default (`MEM_RELEASE` via VirtualFree). When
/// constructed via `FromArenaSlice`, the wrapper is non-owning -- destruction
/// is a no-op, since the slice points into a shared arena page that outlives
/// every Hook.
class ExecutableMemory final {
public:
    ExecutableMemory() = default;
    [[nodiscard]] explicit ExecutableMemory(void* addr, size_t size) noexcept;
    [[nodiscard]] static ExecutableMemory FromArenaSlice(void* addr, size_t size) noexcept;
    ~ExecutableMemory();

    ExecutableMemory(const ExecutableMemory&) = delete;
    ExecutableMemory& operator=(const ExecutableMemory&) = delete;

    ExecutableMemory(ExecutableMemory&& other) noexcept;
    ExecutableMemory& operator=(ExecutableMemory&& other) noexcept;

    [[nodiscard]] uintptr_t Address() const { return reinterpret_cast<uintptr_t>(m_mem); }
    [[nodiscard]] std::byte* BytePtr() const { return static_cast<std::byte*>(m_mem); }
    [[nodiscard]] std::span<std::byte> AsSpan() const {
        return m_mem ? std::span<std::byte>(BytePtr(), m_size) : std::span<std::byte>{};
    }
    [[nodiscard]] bool Valid() const { return m_mem != nullptr; }
    void Release();

private:
    void*  m_mem  = nullptr;
    size_t m_size = 0;
    bool   m_owns = false;  // true => Release frees; false => arena slice (no-op).
};

/// Represents a single hook.
///
/// Address hooks (`Hook(name, RVA, detour)`) resolve immediately via `ResolveAddress()`
/// since they only need the module base.
///
/// Pattern hooks (`Hook(name, ParsedPattern, offset, detour)`) defer resolve so the
/// `HookEngine::ResolveAll()` batch pass can scan `.text` once for all hooks.
/// After `FinalizeResolve(matchAddr)` succeeds, the hook is install-ready.
class Hook final {
public:
    // Pattern hook: stores parsed pattern; FinalizeResolve called later by batch.
    Hook(std::string name, ParsedPattern pattern, int32_t offset, uintptr_t detour);
    // Address hook: RVA-based; ResolveAddress called immediately on register.
    Hook(std::string name, uintptr_t targetRva, uintptr_t detour);
    ~Hook();

    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;
    Hook(Hook&&) noexcept = default;
    Hook& operator=(Hook&&) noexcept = default;

    /// Resolve an address-hook (RVA + module base). Pattern-hooks use FinalizeResolve.
    [[nodiscard]] std::expected<void, std::string> ResolveAddress();
    /// Complete resolution of a pattern-hook given the match address from a batch scan.
    [[nodiscard]] std::expected<void, std::string> FinalizeResolve(uintptr_t matchAddr);
    [[nodiscard]] std::expected<void, std::string> Install();
    void Uninstall();

    [[nodiscard]] bool IsPatternHook() const { return !m_parsedPattern.bytes.empty(); }
    [[nodiscard]] bool IsResolved() const { return m_target != 0 && m_hookLength != 0; }
    [[nodiscard]] bool IsInstalled() const { return m_installed; }
    [[nodiscard]] const std::string& GetName() const { return m_name; }
    [[nodiscard]] uintptr_t GetTarget() const { return m_target; }
    [[nodiscard]] uintptr_t GetTrampoline() const { return m_trampoline.Address(); }
    [[nodiscard]] uintptr_t GetResumeAddress() const { return m_resumeAddr; }
    [[nodiscard]] const ParsedPattern& GetParsedPattern() const { return m_parsedPattern; }

private:
    // Inline buffer for the original instruction bytes saved at the splice
    // site. Bound: kAbsoluteJmpSize (14) <= len <= 14 + max x86-64 instr (15)
    // ~= 29, so 32 is comfortable headroom. Avoids a per-hook heap allocation.
    static constexpr size_t kOriginalBytesCap = 32;

    std::string   m_name;
    ParsedPattern m_parsedPattern;          // cleared after FinalizeResolve
    int32_t       m_patternOffset = 0;
    uintptr_t     m_target = 0;
    uintptr_t     m_detour = 0;
    uintptr_t     m_moduleEnd = 0;          // exclusive upper bound for HDE disassembly
    ExecutableMemory m_trampoline;
    std::array<uint8_t, kOriginalBytesCap> m_originalBytes{};
    uint8_t   m_originalBytesLen = 0;
    size_t    m_hookLength = 0;
    uintptr_t m_resumeAddr = 0;
    bool      m_installed = false;

    /// Shared post-target-set finalization: bounds check, length compute,
    /// resume calc, free parsed pattern.
    [[nodiscard]] std::expected<void, std::string> FinalizeTarget(uintptr_t modBase, uintptr_t modEnd);

    // Default `minLen` matches kAbsoluteJmpSize (14) in hook_engine.cpp.
    [[nodiscard]] std::expected<size_t, std::string> CalculateHookLength(size_t minLen = 14) const;
    [[nodiscard]] std::expected<void, std::string> AllocateTrampoline();
    [[nodiscard]] std::expected<void, std::string> WriteHook(bool enable);
};

class HookEngine final {
public:
    HookEngine() = default;
    ~HookEngine() { Shutdown(); }

    HookEngine(const HookEngine&) = delete;
    HookEngine& operator=(const HookEngine&) = delete;
    HookEngine(HookEngine&&) = delete;
    HookEngine& operator=(HookEngine&&) = delete;

    void Shutdown();

    /// Register a hook by byte pattern. **Does not scan `.text`** -- the actual
    /// resolution is deferred to `ResolveAll()` so all pattern hooks can share
    /// a single linear scan. Only parse-time errors fail this call.
    /// @param pattern  Byte pattern (hex bytes with '?' wildcards).
    /// @param offset   Signed offset added to the match address to get the splice point.
    /// @param detour   Absolute address of the detour routine.
    [[nodiscard]] std::expected<void, std::string> RegisterPatternHook(std::string_view name, std::string_view pattern,
                                                                       int32_t offset, uintptr_t detour);

    /// Register and resolve an RVA-based hook. Module base is added internally.
    /// Resolves immediately since no `.text` scan is needed.
    /// @param targetRva  **RVA** within the game module (must be non-zero).
    /// @param detour     Absolute address of the detour routine.
    [[nodiscard]] std::expected<void, std::string> RegisterAddressHook(std::string_view name, uintptr_t targetRva, uintptr_t detour);

    /// Resolve every still-pending pattern hook in a single linear `.text` scan.
    /// Returns per-hook errors (pattern-not-found, length-too-short, etc.) in
    /// an aggregated vector. Hooks that succeed are install-ready.
    [[nodiscard]] std::expected<void, std::vector<std::string>> ResolveAll();

    /// Install every registered & resolved hook. Per-hook failures are
    /// accumulated and returned as a vector.
    [[nodiscard]] std::expected<void, std::vector<std::string>> InstallAll();
    void UninstallAll();

    [[nodiscard]] bool IsHookInstalled(std::string_view name) const;
    [[nodiscard]] std::optional<uintptr_t> GetTrampoline(std::string_view name) const;
    [[nodiscard]] std::optional<uintptr_t> GetResumeAddress(std::string_view name) const;

    void UnregisterHook(std::string_view name);

private:
    [[nodiscard]] std::expected<void, std::string> InsertHook(Hook&& hook);

    std::flat_map<std::string, Hook, std::less<>> m_hooks;
};

} // namespace jst::core
