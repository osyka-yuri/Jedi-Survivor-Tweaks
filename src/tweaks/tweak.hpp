#pragma once

#include "runtime_control.hpp"
#include <string>
#include <string_view>
#include <expected>
#include <vector>

namespace jst::core {
    class Config;
    class HookEngine;
}

namespace jst::tweaks {

/**
 * Base tweak interface - all tweaks must implement this.
 *
 * Lifecycle (driven by TweakManager):
 *   1. Initialize(hooks, config) -- tweak registers its hooks and applies any
 *      hooks-free state (CVar overrides, etc.). For hook tweaks, this only
 *      *registers* the pattern; the actual resolve is batched in step 2.
 *   2. (TweakManager calls hooks.ResolveAll() once across all tweaks.)
 *   3. FinalizeResolution(hooks) -- hook tweaks read their now-resolved resume
 *      address. Default impl is a no-op for tweaks that don't touch hooks.
 *   4. (TweakManager calls hooks.InstallAll().)
 */
class ITweak {
public:
    virtual ~ITweak() = default;

    // Tweak identification
    [[nodiscard]] virtual std::string_view Name() const = 0;
    [[nodiscard]] virtual std::string_view Description() const = 0;
    [[nodiscard]] virtual bool IsEnabledByDefault() const { return false; }

    // Lifecycle management
    [[nodiscard]] virtual std::expected<void, std::string> Initialize(jst::core::HookEngine& hooks, jst::core::Config& config) = 0;
    /// Called after HookEngine::ResolveAll(); hook-based tweaks read the
    /// resolved continuation address here. Default no-op suits CVar-only tweaks.
    [[nodiscard]] virtual std::expected<void, std::string> FinalizeResolution(
        [[maybe_unused]] jst::core::HookEngine& /*hooks*/) { return {}; }
    /// Called after HookEngine::InstallAll(); hook-based tweaks publish their
    /// initialized state only after every binding in their public group is live.
    [[nodiscard]] virtual std::expected<void, std::string> FinalizeInstallation(
        [[maybe_unused]] jst::core::HookEngine& /*hooks*/) { return {}; }
    virtual void Shutdown() = 0;

    // State query
    [[nodiscard]] virtual bool IsInitialized() const = 0;

    /// Optional: report mutable runtime controls for an overlay. Default:
    /// no controls. Called at most once per overlay frame; safe to allocate.
    /// Controls whose `apply` callbacks mutate runtime state are called from
    /// the render thread -- all current apply targets (context multipliers,
    /// CVar calls) are render-thread-safe.
    [[nodiscard]] virtual std::vector<RuntimeControl> GetRuntimeControls() { return {}; }
};

} // namespace jst::tweaks
