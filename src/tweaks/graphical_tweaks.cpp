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

constexpr int kCAQualityOn = 1;
constexpr int kCAQualityOff = 0;
constexpr int kToneQualityOn = 5;
constexpr int kToneQualityOff = 1;

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

jst::core::CVarQueueResult ApplyVignette(bool enabled) {
    return jst::core::CVarSystem::Instance().SetInt(
        kCVarToneQuality,
        enabled ? kToneQualityOn : kToneQualityOff);
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
    return ITweak::Configure(config);
}

std::expected<void, std::string> GraphicalTweaks::Prepare(
    [[maybe_unused]] jst::core::HookEngine& hooks) {
    const std::array requests{
        jst::core::CVarWriteRequest{
            .name = std::wstring(kCVarSharpen),
            .value = std::format(
                L"{}",
                m_sharpenEnabled ? m_sharpenStrength : 0.0f),
        },
        jst::core::CVarWriteRequest{
            .name = std::wstring(kCVarCAQuality),
            .value = std::to_wstring(
                m_caEnabled ? kCAQualityOn : kCAQualityOff),
        },
        jst::core::CVarWriteRequest{
            .name = std::wstring(kCVarToneQuality),
            .value = std::to_wstring(
                m_vignetteEnabled ? kToneQualityOn : kToneQualityOff),
        },
    };
    auto batch = jst::core::CVarSystem::Instance().QueueBatch(requests);
    if (!batch.Accepted()) {
        return std::unexpected(batch.diagnostic);
    }
    for (size_t i = 0; i < m_tickets.size(); ++i) {
        m_tickets[i] = std::move(batch.commands[i].ticket);
    }

    JST_LOG_INFO(
        "Initialized | sharpening='{} (strength {:.2f})' | "
        "chromaticAberration='{}' | vignette='{}'",
        m_sharpenEnabled ? "on" : "off",
        m_sharpenStrength,
        m_caEnabled ? "on" : "off",
        m_vignetteEnabled ? "on" : "off");
    return {};
}

void GraphicalTweaks::Shutdown() {
    m_tickets = {};
}

std::vector<RuntimeControl> GraphicalTweaks::GetRuntimeControls() {
    std::vector<RuntimeControl> controls;
    controls.reserve(4);

    controls.push_back(CheckboxControl{
        .label = "Sharpening",
        .current = m_sharpenEnabled,
        .defaultValue = true,
        .apply = [this](bool value) {
            const auto result = ApplySharpen(value, m_sharpenStrength);
            if (result.Accepted()) {
                m_sharpenEnabled = value;
                m_tickets[0] = result.ticket;
            }
            return CVarEditResult(result);
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
            if (result.Accepted()) {
                m_sharpenStrength = value;
                m_tickets[0] = result.ticket;
            }
            return CVarEditResult(result);
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
            if (result.Accepted()) {
                m_caEnabled = value;
                m_tickets[1] = result.ticket;
            }
            return CVarEditResult(result);
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
            const auto result = ApplyVignette(value);
            if (result.Accepted()) {
                m_vignetteEnabled = value;
                m_tickets[2] = result.ticket;
            }
            return CVarEditResult(result);
        },
        .persistence = ControlPersistence{
            .section = "Vignette",
            .key = "Enabled",
        },
        .tooltip = "Darkens the corners of the screen.",
    });
    return controls;
}

std::optional<TweakRuntimeStatus> GraphicalTweaks::RuntimeStatus() const {
    return CVarTicketsStatus(m_tickets, TweakRuntimeState::Inactive);
}

} // namespace jst::tweaks
