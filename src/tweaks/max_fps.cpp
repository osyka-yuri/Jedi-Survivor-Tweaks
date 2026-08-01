#include "max_fps.hpp"

#include "core/config.hpp"
#include "core/cvar_system.hpp"
#include "core/logging.hpp"
#include "cvar_runtime_status.hpp"

namespace jst::tweaks {

namespace {

constexpr std::wstring_view kCVarMaxFPS = L"t.MaxFPS";

[[nodiscard]] jst::core::CVarQueueResult QueueMaxFPS(float value) {
    return jst::core::CVarSystem::Instance().SetFloat(kCVarMaxFPS, value);
}

} // namespace

std::expected<TweakActivation, std::string> MaxFPSTweak::Configure(
    const jst::core::Config& config) {
    m_lastCommand.reset();
    m_enabled = config.GetBool(Name(), "Enabled", false);
    m_targetFPS = LoadSliderValue(
        config.GetFloatInRange(
            Name(),
            "TargetFPS",
            kMaxFPSSliderSpec.defaultValue,
            kMaxFPSSliderSpec.min,
            kMaxFPSSliderSpec.max),
        kMaxFPSSliderSpec);
    return TweakActivation{.enabled = m_enabled};
}

std::expected<void, std::string> MaxFPSTweak::Prepare(
    [[maybe_unused]] jst::core::HookEngine& hooks) {
    if (m_enabled) {
        auto result = QueueMaxFPS(m_targetFPS);
        if (!result.Accepted()) {
            return std::unexpected(result.ticket.Snapshot().diagnostic);
        }
        m_lastCommand = std::move(result.ticket);
    }

    JST_LOG_INFO(
        "Initialized | MaxFPS='{}' (target={:.0f} FPS, write={}).",
        m_enabled ? "on" : "off",
        m_targetFPS,
        m_lastCommand ? "queued" : "untouched");
    return {};
}

void MaxFPSTweak::Shutdown() {
    m_lastCommand.reset();
}

std::vector<RuntimeControl> MaxFPSTweak::GetRuntimeControls() {
    return {
        CheckboxControl{
            .label = "Enable FPS Limit",
            .current = m_enabled,
            .defaultValue = false,
            .apply = [this](bool enabled) {
                if (enabled == m_enabled) {
                    return RuntimeEditResult{};
                }
                auto result = QueueMaxFPS(enabled ? m_targetFPS : 0.0f);
                if (result.Accepted()) {
                    m_enabled = enabled;
                    m_lastCommand = result.ticket;
                }
                return CVarEditResult(result);
            },
            .persistence = ControlPersistence{
                .section = "MaxFPS",
                .key = "Enabled",
            },
            .tooltip = "Disabling sets t.MaxFPS to 0 (uncapped).",
        },
        MakeSliderFloatControl(
            kMaxFPSSliderSpec,
            m_targetFPS,
            [this](float value) {
                if (!m_enabled) {
                    m_targetFPS = value;
                    return AppliedEdit();
                }
                auto result = QueueMaxFPS(value);
                if (result.Accepted()) {
                    m_targetFPS = value;
                    m_lastCommand = result.ticket;
                }
                return CVarEditResult(result);
            },
            "Target FPS",
            "MaxFPS",
            "TargetFPS",
            "Maximum frame rate in FPS (0 = uncapped)."),
    };
}

std::optional<TweakRuntimeStatus> MaxFPSTweak::RuntimeStatus() const {
    if (!m_lastCommand) {
        return TweakRuntimeStatus{
            .state = m_enabled ? TweakRuntimeState::Applied
                               : TweakRuntimeState::Inactive,
        };
    }
    return CVarTicketStatus(
        *m_lastCommand,
        m_enabled ? TweakRuntimeState::Applied
                  : TweakRuntimeState::Inactive);
}

} // namespace jst::tweaks
