#pragma once

#include "hook.hpp"

#include <expected>
#include <flat_map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jst::core {

// -----------------------------------------------------------------------------
// HookEngine - transactional x64 hooking with Zydis, near-code gateways,
// and explicit Resume / ReplayOriginal continuation semantics.
//
// See hook_types.hpp for HookSiteSpec, HookError, HookContinuation.
// See gateway_allocator.hpp for the sealed arena implementation.
// -----------------------------------------------------------------------------

class HookEngine final {
public:
    HookEngine() = default;
    ~HookEngine() { Shutdown(); }

    HookEngine(const HookEngine&) = delete;
    HookEngine& operator=(const HookEngine&) = delete;
    HookEngine(HookEngine&&) = delete;
    HookEngine& operator=(HookEngine&&) = delete;

    void Shutdown();

    [[nodiscard]] std::expected<void, HookError>
    RegisterPatternHook(HookSiteSpec spec,
                        std::string_view pattern,
                        int32_t offset,
                        uintptr_t detour);
    [[nodiscard]] std::expected<void, HookError>
    RegisterAddressHook(HookSiteSpec spec, uintptr_t targetRva, uintptr_t detour);

    [[nodiscard]] std::vector<HookError> ResolveAll();
    [[nodiscard]] std::vector<HookError> InstallAll();
    void UninstallAll();

    [[nodiscard]] bool IsHookInstalled(std::string_view name) const;
    [[nodiscard]] bool IsGroupInstalled(std::string_view group) const;
    [[nodiscard]] std::optional<uintptr_t>
    GetContinuationAddress(std::string_view name) const;

    void UnregisterHook(std::string_view name);

private:
    [[nodiscard]] std::expected<void, HookError> InsertHook(Hook&& hook);

    std::flat_map<std::string, Hook, std::less<>> m_hooks;
};

} // namespace jst::core