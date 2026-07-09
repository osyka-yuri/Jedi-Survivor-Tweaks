#include "interpolated_rendering.hpp"
#include "core/cvar_system.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"

namespace jst::tweaks {

namespace {
    constexpr std::wstring_view kCVar = L"respawn.InterpolatedRendering";

    void ApplyInterpolatedRendering(bool on) {
        jst::core::CVarSystem::Instance().SetInt(kCVar, on ? 1 : 0);
    }
} // anonymous namespace

std::expected<void, std::string> InterpolatedRenderingTweak::Initialize(
    [[maybe_unused]] jst::core::HookEngine& hooks, jst::core::Config& config) {
    m_irEnabled = config.GetBool("InterpolatedRendering", "Enabled", true);
    jst::core::CVarSystem::Instance().SetInt(kCVar, m_irEnabled ? 1 : 0);
    m_initialized = true;
    JST_LOG_INFO("Initialized | interpolatedRendering='{}' (queued for async init).", m_irEnabled ? "on" : "off");
    return {};
}

void InterpolatedRenderingTweak::Shutdown() {
    m_initialized = false;
}

std::vector<RuntimeControl> InterpolatedRenderingTweak::GetRuntimeControls() {
    return { CheckboxControl{
        .label         = "Interpolated Rendering",
        .current       = m_irEnabled,
        .defaultValue  = true,
        .apply         = [this](bool v) {
            m_irEnabled = v;
            ApplyInterpolatedRendering(m_irEnabled);
        },
        .configSection = "InterpolatedRendering",
        .configKey     = "Enabled",
    }};
}

} // namespace jst::tweaks
