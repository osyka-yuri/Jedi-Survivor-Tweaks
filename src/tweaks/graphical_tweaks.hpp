#pragma once

#include "tweak.hpp"
#include <string>

namespace jst::tweaks {

class GraphicalTweaks final : public ITweak {
public:
    GraphicalTweaks() = default;
    ~GraphicalTweaks() override = default;

    [[nodiscard]] std::string_view Name() const override { return "GraphicalTweaks"; }
    [[nodiscard]] std::string_view Description() const override { return "Controls sharpening, chromatic aberration, and vignetting settings."; }
    [[nodiscard]] bool IsEnabledByDefault() const override { return true; }

    [[nodiscard]] std::expected<void, std::string> Initialize(jst::core::HookEngine& hooks, jst::core::Config& config) override;
    void Shutdown() override;
    [[nodiscard]] bool IsInitialized() const override { return m_initialized; }

    [[nodiscard]] std::vector<RuntimeControl> GetRuntimeControls() override;

private:
    // All m_*Enabled fields default to false (tweak not active). Initialize()
    // loads the real values from config; the overlay mutates them at runtime.
    bool  m_initialized     = false;
    bool  m_sharpenEnabled  = false;
    float m_sharpenStrength = 1.0f;
    bool  m_caEnabled       = false;
    bool  m_vignetteEnabled = false;
};

} // namespace jst::tweaks
