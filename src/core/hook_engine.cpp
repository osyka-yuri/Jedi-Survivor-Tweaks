#include "hook_engine.hpp"
#include "memory_scanner.hpp"
#include "logging.hpp"
#include <external/hde64/hde64.h>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <format>
#include <mutex>

namespace jst::core {

namespace {

// Absolute indirect jump (`FF 25 00 00 00 00 [8-byte address]`) used to splice
// the detour at every hook site. The hook must consume at least this many bytes
// of original instructions so the rewrite never spills past the saved span.
constexpr size_t kAbsoluteJmpSize = 14;

// Per-hook trampoline slot size: up to ~20 bytes of saved instructions plus a
// 14-byte absolute jmp back to resumeAddr. 64 leaves headroom and gives each
// trampoline its own cache-line-aligned region.
constexpr size_t kTrampolineSize = 64;

// Single shared arena for every trampoline. One VirtualAlloc'd page hosts up
// to 64 slots (4096 / 64). All slots stay RW until InstallAll's final pass
// flips the whole page to PAGE_EXECUTE_READ in one syscall.
constexpr size_t kArenaPageSize = 4096;
struct TrampolineArena {
    void*           base = nullptr;
    size_t          used = 0;
    bool            isExecutable = false;   // true after final RX flip
    std::once_flag  initOnce;
    std::mutex      mtx;                    // protects `used` + isExecutable
};
TrampolineArena g_trampolineArena;

bool EnsureArenaInitialized() {
    bool ok = true;
    std::call_once(g_trampolineArena.initOnce, [&] {
        void* mem = VirtualAlloc(nullptr, kArenaPageSize,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) { ok = false; return; }
        g_trampolineArena.base = mem;
        g_trampolineArena.used = 0;
        g_trampolineArena.isExecutable = false;
    });
    return ok && g_trampolineArena.base != nullptr;
}

class ThreadSuspender final {
public:
    ThreadSuspender() {
        DWORD currentThreadId = GetCurrentThreadId();
        DWORD currentProcessId = GetCurrentProcessId();

        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (hSnapshot == INVALID_HANDLE_VALUE) return;

        THREADENTRY32 te;
        te.dwSize = sizeof(te);

        if (Thread32First(hSnapshot, &te)) {
            do {
                if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID)) {
                    if (te.th32OwnerProcessID == currentProcessId && te.th32ThreadID != currentThreadId) {
                        HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                        if (hThread) {
                            SuspendThread(hThread);
                            m_suspendedThreads.push_back(hThread);
                        }
                    }
                }
                te.dwSize = sizeof(te);
            } while (Thread32Next(hSnapshot, &te));
        }
        CloseHandle(hSnapshot);
    }

    ~ThreadSuspender() {
        for (HANDLE hThread : m_suspendedThreads) {
            ResumeThread(hThread);
            CloseHandle(hThread);
        }
    }

    ThreadSuspender(const ThreadSuspender&) = delete;
    ThreadSuspender& operator=(const ThreadSuspender&) = delete;

private:
    std::vector<HANDLE> m_suspendedThreads;
};

} // anonymous namespace

// ------------------------------------------------------------------
// ExecutableMemory
// ------------------------------------------------------------------
ExecutableMemory::ExecutableMemory(void* addr, size_t size) noexcept
    : m_mem(addr), m_size(size), m_owns(true) {}

ExecutableMemory ExecutableMemory::FromArenaSlice(void* addr, size_t size) noexcept {
    ExecutableMemory m;
    m.m_mem  = addr;
    m.m_size = size;
    m.m_owns = false;
    return m;
}

ExecutableMemory::~ExecutableMemory() {
    Release();
}

ExecutableMemory::ExecutableMemory(ExecutableMemory&& other) noexcept
    : m_mem(other.m_mem), m_size(other.m_size), m_owns(other.m_owns) {
    other.m_mem  = nullptr;
    other.m_size = 0;
    other.m_owns = false;
}

ExecutableMemory& ExecutableMemory::operator=(ExecutableMemory&& other) noexcept {
    if (this != &other) {
        Release();
        m_mem  = other.m_mem;
        m_size = other.m_size;
        m_owns = other.m_owns;
        other.m_mem  = nullptr;
        other.m_size = 0;
        other.m_owns = false;
    }
    return *this;
}

void ExecutableMemory::Release() {
    if (m_mem && m_owns) {
        VirtualFree(m_mem, 0, MEM_RELEASE);
    }
    m_mem  = nullptr;
    m_size = 0;
    m_owns = false;
}

// ------------------------------------------------------------------
// Hook
// ------------------------------------------------------------------
Hook::Hook(std::string name, ParsedPattern pattern, int32_t offset, uintptr_t detour)
    : m_name(std::move(name)),
      m_parsedPattern(std::move(pattern)),
      m_patternOffset(offset),
      m_detour(detour) {}

Hook::Hook(std::string name, uintptr_t targetRva, uintptr_t detour)
    : m_name(std::move(name)), m_target(targetRva), m_detour(detour) {}

Hook::~Hook() {
    Uninstall();
}

std::expected<void, std::string> Hook::ResolveAddress() {
    if (IsPatternHook()) {
        return std::unexpected(std::format("ResolveAddress called on pattern hook '{}'", m_name));
    }
    auto modInfo = GetGameModuleInfo();
    if (!modInfo) {
        return std::unexpected(std::format("Failed to get game module info for hook '{}'", m_name));
    }
    // Address hook: m_target is an RVA. Validate before adding base so
    // RVA=0 doesn't silently become a valid-looking module-base address.
    const uintptr_t rva = m_target;
    if (rva == 0) {
        return std::unexpected(std::format("Invalid address hook RVA (0) for '{}'", m_name));
    }
    m_target = rva + modInfo->base;
    JST_LOG_INFO("Resolved address hook '{}' to absolute 0x{:X}.", m_name, m_target);

    return FinalizeTarget(modInfo->base, modInfo->base + modInfo->size);
}

std::expected<void, std::string> Hook::FinalizeResolve(uintptr_t matchAddr) {
    if (!IsPatternHook()) {
        return std::unexpected(std::format("FinalizeResolve called on address hook '{}'", m_name));
    }
    auto modInfo = GetGameModuleInfo();
    if (!modInfo) {
        return std::unexpected(std::format("Failed to get game module info for hook '{}'", m_name));
    }
    m_target = matchAddr + m_patternOffset;
    JST_LOG_INFO("Resolved hook '{}' at 0x{:X}.", m_name, m_target);
    return FinalizeTarget(modInfo->base, modInfo->base + modInfo->size);
}

std::expected<void, std::string> Hook::FinalizeTarget(uintptr_t modBase, uintptr_t modEnd) {
    m_moduleEnd = modEnd;

    if (m_target == 0 || m_detour == 0) {
        return std::unexpected(std::format("Invalid hook target or detour for '{}'", m_name));
    }
    if (m_target < modBase || m_target >= m_moduleEnd) {
        return std::unexpected(std::format("Hook '{}' target 0x{:X} is outside module bounds", m_name, m_target));
    }

    auto lenRes = CalculateHookLength();
    if (!lenRes) return std::unexpected(lenRes.error());
    m_hookLength = *lenRes;
    if (m_hookLength < kAbsoluteJmpSize) {
        return std::unexpected(std::format("Hook length too short for '{}'", m_name));
    }
    if (m_hookLength > kOriginalBytesCap) {
        return std::unexpected(std::format(
            "Hook '{}' length {} exceeds inline buffer cap {}",
            m_name, m_hookLength, kOriginalBytesCap));
    }
    m_resumeAddr = m_target + m_hookLength;

    // Pattern no longer needed after a successful resolve -- free the buffers.
    m_parsedPattern = ParsedPattern{};
    return {};
}

std::expected<void, std::string> Hook::Install() {
    if (m_installed) return {};
    if (m_target == 0 || m_hookLength < kAbsoluteJmpSize) {
        return std::unexpected(std::format("Hook '{}' not resolved before Install()", m_name));
    }
    if (!m_trampoline.Valid()) {
        auto allocRes = AllocateTrampoline();
        if (!allocRes) return allocRes;
    }
    auto writeRes = WriteHook(true);
    if (!writeRes) {
        m_trampoline.Release();
        return writeRes;
    }
    m_installed = true;
    return {};
}

void Hook::Uninstall() {
    if (!m_installed) return;
    if (m_originalBytesLen != 0) {
        (void)WriteHook(false);
    }
    m_trampoline.Release();
    m_installed = false;
    m_hookLength = 0;
    m_resumeAddr = 0;
    m_originalBytesLen = 0;
}

std::expected<size_t, std::string> Hook::CalculateHookLength(size_t minLen) const {
    size_t length = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(m_target);
    while (length < minLen) {
        if (m_target + length >= m_moduleEnd) {
            return std::unexpected(std::format(
                "Hook '{}' would disassemble past end of module while computing length", m_name));
        }
        hde64s hs;
        const unsigned int instLen = hde64_disasm(p + length, &hs);
        if (instLen == 0 || (hs.flags & F_ERROR)) {
            return std::unexpected(std::format(
                "HDE failed to disassemble instruction at offset {} of hook '{}'", length, m_name));
        }
        length += instLen;
    }
    return length;
}

std::expected<void, std::string> Hook::AllocateTrampoline() {
    if (!EnsureArenaInitialized()) {
        return std::unexpected(std::format("VirtualAlloc failed for trampoline arena (hook '{}')", m_name));
    }
    std::lock_guard lock(g_trampolineArena.mtx);
    if (g_trampolineArena.isExecutable) {
        // Arena was flipped to RX by a prior InstallAll; we don't support
        // re-install / hot-add of new hooks after that. Bail rather than
        // try to write into a non-writable page.
        return std::unexpected(std::format("Trampoline arena is sealed; cannot allocate for '{}'", m_name));
    }
    if (g_trampolineArena.used + kTrampolineSize > kArenaPageSize) {
        return std::unexpected(std::format(
            "Trampoline arena exhausted (used={}/{} bytes) when allocating for '{}'",
            g_trampolineArena.used, kArenaPageSize, m_name));
    }
    void* slot = static_cast<std::byte*>(g_trampolineArena.base) + g_trampolineArena.used;
    g_trampolineArena.used += kTrampolineSize;
    m_trampoline = ExecutableMemory::FromArenaSlice(slot, kTrampolineSize);
    return {};
}

std::expected<void, std::string> Hook::WriteHook(bool enable) {
    if (m_target == 0 || m_detour == 0 || m_hookLength == 0) {
        return std::unexpected(std::format("Invalid state for WriteHook on '{}'", m_name));
    }

    if (m_originalBytesLen == 0) {
        std::memcpy(m_originalBytes.data(), reinterpret_cast<const std::byte*>(m_target), m_hookLength);
        m_originalBytesLen = static_cast<uint8_t>(m_hookLength);
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(m_target), m_hookLength,
                       PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return std::unexpected(std::format("VirtualProtect failed for hook '{}'", m_name));
    }

    if (enable) {
        // FF 25 00 00 00 00  jmp qword ptr [rip+0]  (6 bytes)
        // <8-byte absolute address follows>           => kAbsoluteJmpSize total
        static constexpr uint8_t kJmp[] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
        static_assert(sizeof(kJmp) + sizeof(m_detour) == kAbsoluteJmpSize);
        static constexpr uint8_t kNop = 0x90;

        auto* targetBytes = reinterpret_cast<std::byte*>(m_target);
        std::memcpy(targetBytes, kJmp, sizeof(kJmp));
        std::memcpy(targetBytes + sizeof(kJmp), &m_detour, sizeof(m_detour));
        for (size_t i = kAbsoluteJmpSize; i < m_hookLength; ++i) {
            targetBytes[i] = static_cast<std::byte>(kNop);
        }

        // Trampoline lives in the shared arena page; it stays RW until
        // InstallAll's final batch RX flip. No per-hook VirtualProtect here.
        auto* trampBytes = reinterpret_cast<std::byte*>(m_trampoline.Address());
        std::memcpy(trampBytes, m_originalBytes.data(), m_originalBytesLen);
        auto* trampJmpAddr = trampBytes + m_originalBytesLen;
        std::memcpy(trampJmpAddr, kJmp, sizeof(kJmp));
        std::memcpy(trampJmpAddr + sizeof(kJmp), &m_resumeAddr, sizeof(m_resumeAddr));
    } else {
        std::memcpy(reinterpret_cast<std::byte*>(m_target), m_originalBytes.data(), m_originalBytesLen);
    }

    if (!VirtualProtect(reinterpret_cast<void*>(m_target), m_hookLength, oldProtect, &oldProtect)) {
        JST_LOG_WARNING("VirtualProtect restore failed for hook '{}'.", m_name);
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(m_target), m_hookLength);
    return {};
}

// ------------------------------------------------------------------
// HookEngine
// ------------------------------------------------------------------
void HookEngine::Shutdown() {
    UninstallAll();
    m_hooks.clear();
}

std::expected<void, std::string> HookEngine::RegisterPatternHook(std::string_view name, std::string_view pattern,
                                                                 int32_t offset, uintptr_t detour) {
    auto parsed = ParsePattern(pattern);
    if (!parsed) {
        return std::unexpected(std::format("Pattern parse failed for '{}': {}", name, parsed.error()));
    }
    Hook hook(std::string(name), std::move(*parsed), offset, detour);
    return InsertHook(std::move(hook));
}

std::expected<void, std::string> HookEngine::RegisterAddressHook(std::string_view name, uintptr_t targetRva, uintptr_t detour) {
    Hook hook(std::string(name), targetRva, detour);
    auto resolveRes = hook.ResolveAddress();
    if (!resolveRes) return resolveRes;
    return InsertHook(std::move(hook));
}

std::expected<void, std::vector<std::string>> HookEngine::ResolveAll() {
    // Collect pending pattern-hooks (skip already-resolved address-hooks).
    std::vector<Hook*> pending;
    pending.reserve(m_hooks.size());
    std::vector<ParsedPattern> patterns;   // borrow by const ref into FindPatternsBatch
    patterns.reserve(m_hooks.size());
    for (auto&& [name, hook] : m_hooks) {
        if (hook.IsPatternHook() && !hook.IsResolved()) {
            pending.push_back(&hook);
            patterns.push_back(hook.GetParsedPattern());  // copy parsed pattern out
        }
    }
    if (pending.empty()) return {};

    auto modInfo = GetGameModuleInfo();
    if (!modInfo) {
        return std::unexpected(std::vector<std::string>{"ResolveAll: failed to get game module info"});
    }
    const auto region = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(modInfo->base), modInfo->size);

    auto matches = FindPatternsBatch(std::span<const ParsedPattern>(patterns), region);

    std::vector<std::string> errors;
    for (size_t i = 0; i < pending.size(); ++i) {
        Hook* h = pending[i];
        if (!matches[i]) {
            errors.push_back(std::format("Pattern not found for hook '{}'", h->GetName()));
            continue;
        }
        auto res = h->FinalizeResolve(*matches[i]);
        if (!res) {
            errors.push_back(std::move(res).error());
        }
    }
    if (!errors.empty()) {
        return std::unexpected(std::move(errors));
    }
    return {};
}

std::expected<void, std::string> HookEngine::InsertHook(Hook&& hook) {
    if (m_hooks.contains(hook.GetName())) {
        return std::unexpected(std::format("Hook '{}' already registered.", hook.GetName()));
    }
    m_hooks.emplace(hook.GetName(), std::move(hook));
    return {};
}

std::expected<void, std::vector<std::string>> HookEngine::InstallAll() {
    ThreadSuspender suspender;
    std::vector<std::string> errors;
    size_t installedCount = 0;
    for (auto&& [name, hook] : m_hooks) {
        if (!hook.IsInstalled()) {
            auto installRes = hook.Install();
            if (!installRes) {
                errors.push_back(std::move(installRes).error());
            } else {
                ++installedCount;
                JST_LOG_INFO("Installed hook '{}' | target=0x{:X} | trampoline=0x{:X} | resume=0x{:X}",
                             hook.GetName(), hook.GetTarget(), hook.GetTrampoline(), hook.GetResumeAddress());
            }
        }
    }

    // Single W^X transition for the whole trampoline arena. Per-hook VirtualProtect
    // calls on the arena have been removed -- the arena was RW for the entire
    // write phase above, and is now flipped to PAGE_EXECUTE_READ exactly once.
    if (installedCount > 0) {
        std::lock_guard lock(g_trampolineArena.mtx);
        if (g_trampolineArena.base && !g_trampolineArena.isExecutable) {
            DWORD oldProtect = 0;
            if (!VirtualProtect(g_trampolineArena.base, kArenaPageSize,
                                PAGE_EXECUTE_READ, &oldProtect)) {
                JST_LOG_ERROR("Trampoline arena RX flip failed (err={}); page left RW. "
                              "Detours will still execute on most CPUs (DEP-tolerant) "
                              "but this is a W^X violation.", GetLastError());
            } else {
                g_trampolineArena.isExecutable = true;
                FlushInstructionCache(GetCurrentProcess(),
                                      g_trampolineArena.base, kArenaPageSize);
            }
        }
    }

    if (!errors.empty()) {
        return std::unexpected(std::move(errors));
    }
    return {};
}

void HookEngine::UninstallAll() {
    ThreadSuspender suspender;
    for (auto&& [name, hook] : m_hooks) {
        if (hook.IsInstalled()) {
            hook.Uninstall();
            JST_LOG_INFO("Uninstalled hook '{}'.", hook.GetName());
        }
    }
}

bool HookEngine::IsHookInstalled(std::string_view name) const {
    auto it = m_hooks.find(name);
    return it != m_hooks.end() && it->second.IsInstalled();
}

std::optional<uintptr_t> HookEngine::GetTrampoline(std::string_view name) const {
    auto it = m_hooks.find(name);
    if (it == m_hooks.end()) return std::nullopt;
    uintptr_t addr = it->second.GetTrampoline();
    return addr != 0 ? std::optional{addr} : std::nullopt;
}

std::optional<uintptr_t> HookEngine::GetResumeAddress(std::string_view name) const {
    auto it = m_hooks.find(name);
    if (it == m_hooks.end()) return std::nullopt;
    uintptr_t addr = it->second.GetResumeAddress();
    return addr != 0 ? std::optional{addr} : std::nullopt;
}

void HookEngine::UnregisterHook(std::string_view name) {
    auto it = m_hooks.find(name);
    if (it != m_hooks.end()) {
        it->second.Uninstall();
        m_hooks.erase(it);
    }
}

} // namespace jst::core