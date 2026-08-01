#pragma once

#include "core/cvar_system.hpp"
#include "tweak.hpp"

namespace jst::tweaks {

class InterpolatedRenderingTweak final : public ITweak {
public:
    [[nodiscard]] std::string_view Name() const noexcept override {
        return "InterpolatedRendering";
    }
    [[nodiscard]] std::string_view Description() const noexcept override {
        return "Opt-in frame interpolation intended to reduce CPU stutters and camera jitter.";
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
    bool m_overrideOwned = false;
    jst::core::CVarCommandTicket m_ticket;
};

} // namespace jst::tweaks
