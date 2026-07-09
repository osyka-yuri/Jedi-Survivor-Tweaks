#include "graphical_tweaks.hpp"
#include "core/cvar_system.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include "slider_specs.hpp"
#include <format>

namespace jst::tweaks {

namespace {
    // Engine CVar names referenced by this tweak. Resolved in one batch pass.
    constexpr std::wstring_view kCVarSharpen   = L"r.Tonemapper.Sharpen";
    constexpr std::wstring_view kCVarCAQuality = L"r.SceneColorFringeQuality";
    constexpr std::wstring_view kCVarToneQual  = L"r.Tonemapper.Quality";

    // CVar value mappings. The on/off pair is captured here so Initialize and
    // the runtime apply lambdas can never disagree on what "on" or "off" means.
    constexpr int kCAQualityOn   = 1;  // game default
    constexpr int kCAQualityOff  = 0;
    constexpr int kToneQualOn    = 5;  // game default (vignette enabled)
    constexpr int kToneQualOff   = 1;  // tonemap quality at which vignette is suppressed

    // Single source of truth for each runtime toggle's effect.
    void ApplySharpen(bool on, float strength) {
        jst::core::CVarSystem::Instance().SetFloat(kCVarSharpen, on ? strength : 0.0f);
    }
    void ApplyChromaticAberration(bool on) {
        jst::core::CVarSystem::Instance().SetInt(kCVarCAQuality, on ? kCAQualityOn : kCAQualityOff);
    }
    void ApplyVignette(bool on) {
        jst::core::CVarSystem::Instance().SetInt(kCVarToneQual, on ? kToneQualOn : kToneQualOff);
    }
} // anonymous namespace

std::expected<void, std::string> GraphicalTweaks::Initialize(
    [[maybe_unused]] jst::core::HookEngine& hooks, jst::core::Config& config) {
    // Read config, clamping the sharpening strength to the slider range so
    // the runtime control matches the loaded state.
    m_sharpenEnabled  = config.GetBool ("Sharpening",          "Enabled",  true);
    m_sharpenStrength = LoadSliderValue(
        config.GetFloat("Sharpening", "Strength", kSharpenSliderSpec.defaultValue),
        kSharpenSliderSpec);
    m_caEnabled       = config.GetBool ("ChromaticAberration", "Enabled",  true);
    m_vignetteEnabled = config.GetBool ("Vignette",            "Enabled",  true);

    // Always apply the current state. Doing this unconditionally (rather than
    // only when the section is disabled) keeps the runtime path and startup
    // path observably identical -- both go through the Apply* helpers.
    ApplySharpen(m_sharpenEnabled, m_sharpenStrength);
    ApplyChromaticAberration(m_caEnabled);
    ApplyVignette(m_vignetteEnabled);

    m_initialized = true;
    JST_LOG_INFO("Initialized | sharpening='{} (strength {:.2f})' | chromaticAberration='{}' | vignette='{}'",
                 m_sharpenEnabled ? "on" : "off",
                 m_sharpenStrength,
                 m_caEnabled ? "on" : "off",
                 m_vignetteEnabled ? "on" : "off");
    return {};
}

void GraphicalTweaks::Shutdown() {
    m_initialized = false;
}

std::vector<RuntimeControl> GraphicalTweaks::GetRuntimeControls() {
    std::vector<RuntimeControl> controls;
    controls.reserve(4);  // sharpen toggle + strength slider + CA toggle + vignette toggle

    controls.push_back(CheckboxControl{
        .label         = "Sharpening",
        .current       = m_sharpenEnabled,
        .defaultValue  = true,
        .apply         = [this](bool v) {
            m_sharpenEnabled = v;
            ApplySharpen(m_sharpenEnabled, m_sharpenStrength);
        },
        .configSection = "Sharpening",
        .configKey     = "Enabled",
        .tooltip       = "Enable or disable the post-process sharpening filter.",
    });

    controls.push_back(MakeSliderFloatControl(
        kSharpenSliderSpec,
        m_sharpenStrength,
        [this](float value) {
            m_sharpenStrength = value;
            ApplySharpen(m_sharpenEnabled, m_sharpenStrength);
        },
        "Sharpening Strength",
        "Sharpening",
        "Strength",
        "0 = off, 1 = default, 10 = very strong. Only active when sharpening is enabled."));

    controls.push_back(CheckboxControl{
        .label         = "Chromatic Aberration",
        .current       = m_caEnabled,
        .defaultValue  = true,
        .apply         = [this](bool v) {
            m_caEnabled = v;
            ApplyChromaticAberration(m_caEnabled);
        },
        .configSection = "ChromaticAberration",
        .configKey     = "Enabled",
        .tooltip       = "Color fringing at screen edges. Disable for a cleaner image.",
    });

    controls.push_back(CheckboxControl{
        .label         = "Vignette",
        .current       = m_vignetteEnabled,
        .defaultValue  = true,
        .apply         = [this](bool v) {
            m_vignetteEnabled = v;
            ApplyVignette(m_vignetteEnabled);
        },
        .configSection = "Vignette",
        .configKey     = "Enabled",
        .tooltip       = "Darkens the corners of the screen. Disable for a uniform look.",
    });

    return controls;
}

} // namespace jst::tweaks
