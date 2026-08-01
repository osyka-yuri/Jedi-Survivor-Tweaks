#pragma once

#include "core/config.hpp"
#include "runtime_control.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jst::core {
class HookEngine;
}

namespace jst::tweaks {

enum class TweakActivationMode : uint8_t {
    LaunchGated,
    Runtime,
};

struct TweakActivation {
    bool enabled = false;
};

enum class TweakRuntimeState : uint8_t {
    Inactive,
    Pending,
    Applied,
    Failed,
};

struct TweakRuntimeStatus {
    TweakRuntimeState state = TweakRuntimeState::Applied;
    std::string message;
};

class ITweak {
public:
    virtual ~ITweak() = default;

    [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
    [[nodiscard]] virtual std::string_view Description() const noexcept = 0;
    [[nodiscard]] virtual bool IsEnabledByDefault() const noexcept {
        return false;
    }
    [[nodiscard]] virtual TweakActivationMode ActivationMode() const noexcept {
        return TweakActivationMode::LaunchGated;
    }

    [[nodiscard]] virtual std::expected<TweakActivation, std::string> Configure(
        const jst::core::Config& config) {
        return TweakActivation{
            .enabled = config.GetBool(
                Name(), "Enabled", IsEnabledByDefault()),
        };
    }
    [[nodiscard]] virtual std::expected<void, std::string> Prepare(
        jst::core::HookEngine& hooks) = 0;
    [[nodiscard]] virtual std::expected<void, std::string> FinalizeResolution(
        [[maybe_unused]] jst::core::HookEngine& hooks) {
        return {};
    }
    [[nodiscard]] virtual std::expected<void, std::string> FinalizeInstallation(
        [[maybe_unused]] jst::core::HookEngine& hooks) {
        return {};
    }
    virtual void Shutdown() = 0;

    [[nodiscard]] virtual std::vector<RuntimeControl> GetRuntimeControls() {
        return {};
    }
    [[nodiscard]] virtual RuntimeControlResetResult ResetRuntimeControls(
        jst::core::Config&) {
        return RuntimeControlResetResult::Unsupported;
    }
    [[nodiscard]] virtual std::optional<TweakRuntimeStatus> RuntimeStatus()
        const {
        return std::nullopt;
    }
};

} // namespace jst::tweaks
