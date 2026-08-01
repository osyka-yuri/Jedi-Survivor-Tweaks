#pragma once

#include "core/cvar_system.hpp"
#include "slider_specs.hpp"
#include "tweak.hpp"

#include <optional>

namespace jst::tweaks {

class MaxFPSTweak final : public ITweak {
public:
    [[nodiscard]] std::string_view Name() const noexcept override {
        return "MaxFPS";
    }
    [[nodiscard]] std::string_view Description() const noexcept override {
        return "Limits the maximum frame rate through t.MaxFPS.";
    }
    [[nodiscard]] bool IsEnabledByDefault() const noexcept override {
        return false;
    }
    [[nodiscard]] TweakActivationMode ActivationMode() const noexcept override {
        return TweakActivationMode::Runtime;
    }

    [[nodiscard]] std::expected<TweakActivation, std::string> Configure(
        const jst::core::Config& config) override;
    [[nodiscard]] std::expected<void, std::string> Prepare(
        jst::core::HookEngine& hooks) override;
    void Shutdown() override;

    [[nodiscard]] std::vector<RuntimeControl> GetRuntimeControls() override;
    [[nodiscard]] std::optional<TweakRuntimeStatus> RuntimeStatus()
        const override;

private:
    bool m_enabled = false;
    float m_targetFPS = 60.0f;
    std::optional<jst::core::CVarCommandTicket> m_lastCommand;
};

} // namespace jst::tweaks
