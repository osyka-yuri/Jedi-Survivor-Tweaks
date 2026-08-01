#pragma once

#include "core/cvar_system.hpp"
#include "tweak.hpp"

#include <array>

namespace jst::tweaks {

class GraphicalTweaks final : public ITweak {
public:
    [[nodiscard]] std::string_view Name() const noexcept override {
        return "GraphicalTweaks";
    }
    [[nodiscard]] std::string_view Description() const noexcept override {
        return "Controls sharpening, chromatic aberration, and vignetting settings.";
    }
    [[nodiscard]] bool IsEnabledByDefault() const noexcept override {
        return true;
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
    bool m_sharpenEnabled = false;
    float m_sharpenStrength = 1.0f;
    bool m_caEnabled = false;
    bool m_vignetteEnabled = false;
    std::array<jst::core::CVarCommandTicket, 3> m_tickets;
};

} // namespace jst::tweaks
