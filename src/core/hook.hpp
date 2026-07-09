#pragma once

#include "hook_types.hpp"
#include "memory_scanner.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

namespace jst::core {

class Hook final {
public:
    Hook(HookSiteSpec spec,
         ParsedPattern pattern,
         int32_t patternOffset,
         uintptr_t detour);
    Hook(HookSiteSpec spec, uintptr_t targetRva, uintptr_t detour);
    ~Hook();

    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;
    Hook(Hook&&) noexcept = default;
    Hook& operator=(Hook&&) noexcept = default;

    [[nodiscard]] std::expected<void, HookError> ResolveAddress();
    [[nodiscard]] std::expected<void, HookError> FinalizeResolve(uintptr_t matchAddress);
    [[nodiscard]] std::expected<void, HookError> ValidatePrepare();
    [[nodiscard]] std::expected<void, HookError> Prepare();
    [[nodiscard]] std::expected<void, HookError> Install();
    [[nodiscard]] std::expected<void, HookError> Uninstall();

    [[nodiscard]] bool IsPatternHook() const noexcept {
        return !m_parsedPattern.bytes.empty();
    }
    [[nodiscard]] bool IsResolved() const noexcept {
        return m_target != 0 && m_overwriteLength != 0;
    }
    [[nodiscard]] bool IsPrepared() const noexcept { return m_prepared; }
    [[nodiscard]] bool IsInstalled() const noexcept { return m_installed; }
    [[nodiscard]] bool NeedsInstallRollback() const noexcept { return m_installRollbackPending; }
    [[nodiscard]] const std::string& Name() const noexcept { return m_spec.name; }
    [[nodiscard]] const std::string& Group() const noexcept { return m_spec.group; }
    [[nodiscard]] uintptr_t Target() const noexcept { return m_target; }
    [[nodiscard]] uintptr_t Gateway() const noexcept { return m_gateway.Address(); }
    [[nodiscard]] uintptr_t ResumeAddress() const noexcept { return m_resumeAddress; }
    [[nodiscard]] uintptr_t ContinuationAddress() const noexcept;
    [[nodiscard]] const ParsedPattern& Pattern() const noexcept { return m_parsedPattern; }

private:
    static constexpr size_t kOriginalBytesCapacity = 32;

    [[nodiscard]] std::expected<void, HookError>
    FinalizeTarget(uintptr_t moduleBase, uintptr_t moduleEnd);
    [[nodiscard]] std::expected<void, HookError>
    WriteTarget(std::span<const std::byte> bytes);

    HookSiteSpec m_spec;
    ParsedPattern m_parsedPattern;
    int32_t m_patternOffset = 0;
    uintptr_t m_target = 0;
    uintptr_t m_detour = 0;
    ExecutableMemory m_gateway;
    std::array<std::byte, kOriginalBytesCapacity> m_originalBytes{};
    uint8_t m_originalBytesLength = 0;
    size_t m_overwriteLength = 0;
    uintptr_t m_resumeAddress = 0;
    uint32_t m_originalProtection = 0;
    std::optional<int32_t> m_cachedRel32;
    bool m_prepared = false;
    bool m_installed = false;
    bool m_installRollbackPending = false;
};

} // namespace jst::core