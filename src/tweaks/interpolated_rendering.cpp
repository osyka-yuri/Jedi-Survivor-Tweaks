#include "interpolated_rendering.hpp"

#include "core/config.hpp"
#include "core/logging.hpp"
#include "cvar_runtime_status.hpp"

namespace jst::tweaks {

namespace {

constexpr std::wstring_view kCVar = L"respawn.InterpolatedRendering";

jst::core::CVarQueueResult QueueInterpolatedRendering(bool enabled) {
    return jst::core::CVarSystem::Instance().SetInt(kCVar, enabled ? 1 : 0);
}

} // namespace

std::expected<TweakActivation, std::string>
InterpolatedRenderingTweak::Configure(const jst::core::Config& config) {
    m_overrideOwned = false;
    m_ticket = {};
    m_enabled = config.GetBool(Name(), "Enabled", false);
    return TweakActivation{.enabled = m_enabled};
}

std::expected<void, std::string> InterpolatedRenderingTweak::Prepare(
    [[maybe_unused]] jst::core::HookEngine& hooks) {
    if (m_enabled) {
        const auto result = QueueInterpolatedRendering(true);
        if (!result.Accepted()) {
            return std::unexpected(result.ticket.Snapshot().diagnostic);
        }
        m_overrideOwned = true;
        m_ticket = result.ticket;
    }
    JST_LOG_INFO(
        "Initialized | interpolatedRendering='{}' (write={}).",
        m_enabled ? "on" : "off",
        m_overrideOwned ? "queued" : "untouched");
    return {};
}

void InterpolatedRenderingTweak::Shutdown() {
    m_overrideOwned = false;
    m_ticket = {};
}

std::vector<RuntimeControl>
InterpolatedRenderingTweak::GetRuntimeControls() {
    return {CheckboxControl{
        .label = "Interpolated Rendering",
        .current = m_enabled,
        .defaultValue = false,
        .apply = [this](bool value) {
            if (value == m_enabled) {
                return RuntimeEditResult{};
            }
            if (!value && !m_overrideOwned) {
                m_enabled = false;
                return AppliedEdit();
            }
            const auto result = QueueInterpolatedRendering(value);
            if (result.Accepted()) {
                m_enabled = value;
                m_overrideOwned = m_overrideOwned || value;
                m_ticket = result.ticket;
            }
            return CVarEditResult(result);
        },
        .persistence = ControlPersistence{
            .section = "InterpolatedRendering",
            .key = "Enabled",
        },
        .tooltip = "Disabled by default; enable to use the game's interpolated-rendering setting.",
    }};
}

std::optional<TweakRuntimeStatus>
InterpolatedRenderingTweak::RuntimeStatus() const {
    if (!m_ticket) {
        if (m_enabled) {
            return std::nullopt;
        }
        return TweakRuntimeStatus{.state = TweakRuntimeState::Inactive};
    }
    return CVarTicketStatus(
        m_ticket,
        m_enabled ? TweakRuntimeState::Applied
                  : TweakRuntimeState::Inactive);
}

} // namespace jst::tweaks
