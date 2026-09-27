#include "graphical_tweaks.hpp"

#include "core/config.hpp"
#include "core/logging.hpp"
#include "cvar_runtime_status.hpp"
#include "slider_specs.hpp"

#include <format>

namespace jst::tweaks {

namespace {

constexpr std::wstring_view kCVarSharpen = L"r.Tonemapper.Sharpen";
constexpr std::wstring_view kCVarCAQuality = L"r.SceneColorFringeQuality";
constexpr std::wstring_view kCVarToneQuality = L"r.Tonemapper.Quality";
constexpr std::wstring_view kCVarDepthOfFieldQuality =
    L"r.DepthOfFieldQuality";
constexpr std::wstring_view kCVarMotionBlurQuality = L"r.MotionBlurQuality";

constexpr int kCAQualityOn = 1;
constexpr int kCAQualityOff = 0;
constexpr int kToneQualityVignetteOff = 1;
constexpr int kToneQualityVignetteOnGrainOff = 3;
constexpr int kToneQualityVignetteOnGrainOn = 5;
constexpr int kDepthOfFieldQualityOn = 2;
constexpr int kDepthOfFieldQualityOff = 0;
constexpr int kMotionBlurQualityOn = 4;
constexpr int kMotionBlurQualityOff = 0;

constexpr int TonemapperQuality(bool vignetteEnabled, bool filmGrainEnabled) {
    if (!vignetteEnabled) {
        return kToneQualityVignetteOff;
    }
    return filmGrainEnabled
        ? kToneQualityVignetteOnGrainOn
        : kToneQualityVignetteOnGrainOff;
}

jst::core::CVarQueueResult ApplySharpen(bool enabled, float strength) {
    return jst::core::CVarSystem::Instance().SetFloat(
        kCVarSharpen,
        enabled ? strength : 0.0f);
}

jst::core::CVarQueueResult ApplyChromaticAberration(bool enabled) {
    return jst::core::CVarSystem::Instance().SetInt(
        kCVarCAQuality,
        enabled ? kCAQualityOn : kCAQualityOff);
}

jst::core::CVarQueueResult ApplyTonemapperQuality(
    bool vignetteEnabled,
    bool filmGrainEnabled) {
    return jst::core::CVarSystem::Instance().SetInt(
        kCVarToneQuality,
        TonemapperQuality(vignetteEnabled, filmGrainEnabled));
}

jst::core::CVarQueueResult ApplyDepthOfField(bool enabled) {
    return jst::core::CVarSystem::Instance().SetInt(
        kCVarDepthOfFieldQuality,
        enabled ? kDepthOfFieldQualityOn : kDepthOfFieldQualityOff);
}

jst::core::CVarQueueResult ApplyMotionBlur(bool enabled) {
    return jst::core::CVarSystem::Instance().SetInt(
        kCVarMotionBlurQuality,
        enabled ? kMotionBlurQualityOn : kMotionBlurQualityOff);
}

} // namespace

std::expected<TweakActivation, std::string> GraphicalTweaks::Configure(
    const jst::core::Config& config) {
    m_tickets = {};
    m_sharpenEnabled = config.GetBool("Sharpening", "Enabled", true);
    m_sharpenStrength = LoadSliderValue(
        config.GetFloatInRange(
            "Sharpening",
            "Strength",
            kSharpenSliderSpec.defaultValue,
            kSharpenSliderSpec.min,
            kSharpenSliderSpec.max),
        kSharpenSliderSpec);
    m_caEnabled = config.GetBool("ChromaticAberration", "Enabled", true);
    m_vignetteEnabled = config.GetBool("Vignette", "Enabled", true);
    m_depthOfFieldEnabled = config.GetBool("DepthOfField", "Enabled", true);
    m_motionBlurEnabled = config.GetBool("MotionBlur", "Enabled", true);
    m_filmGrainEnabled = config.GetBool("FilmGrain", "Enabled", false);
    return ITweak::Configure(config);
}

std::expected<void, std::string> GraphicalTweaks::Prepare(
    [[maybe_unused]] jst::core::HookEngine& hooks) {
    const std::array requests{
        jst::core::CVarWriteRequest{
            .name = std::wstring(kCVarSharpen),
            .value = m_sharpenEnabled ? m_sharpenStrength : 0.0f,
        },
        jst::core::CVarWriteRequest{
            .name = std::wstring(kCVarCAQuality),
            .value = m_caEnabled ? kCAQualityOn : kCAQualityOff,
        },
        jst::core::CVarWriteRequest{
            .name = std::wstring(kCVarToneQuality),
            .value = TonemapperQuality(
                m_vignetteEnabled,
                m_filmGrainEnabled),
        },
        jst::core::CVarWriteRequest{
            .name = std::wstring(kCVarDepthOfFieldQuality),
            .value = m_depthOfFieldEnabled
                ? kDepthOfFieldQualityOn
                : kDepthOfFieldQualityOff,
        },
        jst::core::CVarWriteRequest{
            .name = std::wstring(kCVarMotionBlurQuality),
            .value = m_motionBlurEnabled
                ? kMotionBlurQualityOn
                : kMotionBlurQualityOff,
        },
    };
    auto batch = jst::core::CVarSystem::Instance().QueueBatch(requests);
    if (!batch.Accepted()) {
        return std::unexpected(batch.diagnostic);
    }
    for (std::size_t i = 0; i < m_tickets.size(); ++i) {
        m_tickets[i] = std::move(batch.commands[i].ticket);
    }

    JST_LOG_INFO(
        "Initialized | sharpening='{} (strength {:.2f})' | "
        "chromaticAberration='{}' | vignette='{}' | depthOfField='{}' | "
        "motionBlur='{}' | filmGrain='{}'",
        m_sharpenEnabled ? "on" : "off",
        m_sharpenStrength,
        m_caEnabled ? "on" : "off",
        m_vignetteEnabled ? "on" : "off",
        m_depthOfFieldEnabled ? "on" : "off",
        m_motionBlurEnabled ? "on" : "off",
        m_filmGrainEnabled ? "on" : "off");
    return {};
}

void GraphicalTweaks::Shutdown() {
    m_tickets = {};
}

std::vector<RuntimeControl> GraphicalTweaks::GetRuntimeControls() {
    std::vector<RuntimeControl> controls;
    controls.reserve(7);

    controls.push_back(CheckboxControl{
        .label = "Sharpening",
        .current = m_sharpenEnabled,
        .defaultValue = true,
        .apply = [this](bool value) {
            const auto result = ApplySharpen(value, m_sharpenStrength);
            const auto edit = CVarEditResult(result);
            if (edit.ShouldPersist()) {
                m_sharpenEnabled = value;
                m_tickets[ToIndex(TicketSlot::Sharpening)] = result.ticket;
            }
            return edit;
        },
        .persistence = ControlPersistence{
            .section = "Sharpening",
            .key = "Enabled",
        },
        .tooltip = "Enable or disable the post-process sharpening filter.",
    });

    controls.push_back(MakeSliderFloatControl(
        kSharpenSliderSpec,
        m_sharpenStrength,
        [this](float value) {
            const auto result = ApplySharpen(m_sharpenEnabled, value);
            const auto edit = CVarEditResult(result);
            if (edit.ShouldPersist()) {
                m_sharpenStrength = value;
                m_tickets[ToIndex(TicketSlot::Sharpening)] = result.ticket;
            }
            return edit;
        },
        "Sharpening Strength",
        "Sharpening",
        "Strength",
        "0 = off, 1 = default, 10 = very strong."));

    controls.push_back(CheckboxControl{
        .label = "Chromatic Aberration",
        .current = m_caEnabled,
        .defaultValue = true,
        .apply = [this](bool value) {
            const auto result = ApplyChromaticAberration(value);
            const auto edit = CVarEditResult(result);
            if (edit.ShouldPersist()) {
                m_caEnabled = value;
                m_tickets[ToIndex(TicketSlot::ChromaticAberration)] =
                    result.ticket;
            }
            return edit;
        },
        .persistence = ControlPersistence{
            .section = "ChromaticAberration",
            .key = "Enabled",
        },
        .tooltip = "Color fringing at screen edges.",
    });

    controls.push_back(CheckboxControl{
        .label = "Vignette",
        .current = m_vignetteEnabled,
        .defaultValue = true,
        .apply = [this](bool value) {
            const auto result = ApplyTonemapperQuality(
                value,
                m_filmGrainEnabled);
            const auto edit = CVarEditResult(result);
            if (edit.ShouldPersist()) {
                m_vignetteEnabled = value;
                m_tickets[ToIndex(TicketSlot::Tonemapper)] = result.ticket;
            }
            return edit;
        },
        .persistence = ControlPersistence{
            .section = "Vignette",
            .key = "Enabled",
        },
        .tooltip = "Darkens the corners of the screen.",
    });

    controls.push_back(CheckboxControl{
        .label = "Depth of Field",
        .current = m_depthOfFieldEnabled,
        .defaultValue = true,
        .apply = [this](bool value) {
            const auto result = ApplyDepthOfField(value);
            const auto edit = CVarEditResult(result);
            if (edit.ShouldPersist()) {
                m_depthOfFieldEnabled = value;
                m_tickets[ToIndex(TicketSlot::DepthOfField)] = result.ticket;
            }
            return edit;
        },
        .persistence = ControlPersistence{
            .section = "DepthOfField",
            .key = "Enabled",
        },
        .tooltip = "Enable or disable the game's depth-of-field effect.",
    });

    controls.push_back(CheckboxControl{
        .label = "Motion Blur",
        .current = m_motionBlurEnabled,
        .defaultValue = true,
        .apply = [this](bool value) {
            const auto result = ApplyMotionBlur(value);
            const auto edit = CVarEditResult(result);
            if (edit.ShouldPersist()) {
                m_motionBlurEnabled = value;
                m_tickets[ToIndex(TicketSlot::MotionBlur)] = result.ticket;
            }
            return edit;
        },
        .persistence = ControlPersistence{
            .section = "MotionBlur",
            .key = "Enabled",
        },
        .tooltip = "Enable or disable camera and object motion blur.",
    });

    controls.push_back(CheckboxControl{
        .label = "Film Grain",
        .current = m_filmGrainEnabled,
        .defaultValue = false,
        .apply = [this](bool value) {
            const auto result = ApplyTonemapperQuality(
                m_vignetteEnabled,
                value);
            const auto edit = CVarEditResult(result);
            if (edit.ShouldPersist()) {
                m_filmGrainEnabled = value;
                m_tickets[ToIndex(TicketSlot::Tonemapper)] = result.ticket;
            }
            return edit;
        },
        .persistence = ControlPersistence{
            .section = "FilmGrain",
            .key = "Enabled",
        },
        .tooltip = "Allow the engine film-grain pass. Requires Vignette; "
                   "scene settings may still suppress visible grain.",
    });
    return controls;
}

std::optional<TweakRuntimeStatus> GraphicalTweaks::RuntimeStatus() const {
    return CVarTicketsStatus(m_tickets, TweakRuntimeState::Inactive);
}

} // namespace jst::tweaks
