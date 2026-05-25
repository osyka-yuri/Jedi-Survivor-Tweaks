#include "graphical_tweaks.hpp"
#include "core/cvar_system.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include <algorithm>
#include <array>
#include <format>

namespace jst::tweaks {

std::expected<void, std::string> GraphicalTweaks::Initialize(
    [[maybe_unused]] jst::core::HookEngine& hooks, jst::core::Config& config) {
    auto& cvarSys = jst::core::CVarSystem::Instance();

    // Batch-resolve all referenced cvars in a single .text scan.
    constexpr std::array<std::wstring_view, 3> kCVars{
        L"r.Tonemapper.Sharpen",
        L"r.SceneColorFringeQuality",
        L"r.Tonemapper.Quality",
    };
    cvarSys.ResolveBatch(kCVars);

    std::string sharpenMsg = "Not modified";
    bool caDisabled = false;
    bool vignetteDisabled = false;

    // [Sharpening]
    if (config.HasSection("Sharpening")) {
        const bool enabled = config.GetBool("Sharpening", "Enabled", true);
        if (enabled) {
            float strength = config.GetFloat("Sharpening", "Strength", 1.0f);
            strength = std::clamp(strength, 0.0f, 10.0f);
            if (cvarSys.SetFloat(L"r.Tonemapper.Sharpen", strength)) {
                sharpenMsg = std::format("Set to {:.2f}", strength);
            } else {
                sharpenMsg = "Failed to set";
            }
        } else {
            sharpenMsg = "Disabled in config";
        }
    }

    // [ChromaticAberration]
    if (config.HasSection("ChromaticAberration")) {
        const bool caEnabled = config.GetBool("ChromaticAberration", "Enabled", true);
        if (!caEnabled && cvarSys.SetInt(L"r.SceneColorFringeQuality", 0)) {
            caDisabled = true;
        }
    }

    // [Vignette]
    if (config.HasSection("Vignette")) {
        const bool vignetteEnabled = config.GetBool("Vignette", "Enabled", true);
        if (!vignetteEnabled && cvarSys.SetInt(L"r.Tonemapper.Quality", 1)) {
            vignetteDisabled = true;
        }
    }

    m_initialized = true;
    JST_LOG_INFO("Initialized | sharpening='{}' | chromaticAberration='{}' | vignette='{}'",
                 sharpenMsg,
                 caDisabled ? "disabled" : "unchanged",
                 vignetteDisabled ? "disabled" : "unchanged");
    return {};
}

void GraphicalTweaks::Shutdown() {
    m_initialized = false;
}

} // namespace jst::tweaks
