#include "interpolated_rendering.hpp"
#include "core/cvar_system.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include <array>

namespace jst::tweaks {

std::expected<void, std::string> InterpolatedRenderingTweak::Initialize(
    [[maybe_unused]] jst::core::HookEngine& hooks,
    [[maybe_unused]] jst::core::Config& config) {
    auto& cvarSys = jst::core::CVarSystem::Instance();
    constexpr std::array<std::wstring_view, 1> kCVars{L"respawn.InterpolatedRendering"};
    cvarSys.ResolveBatch(kCVars);

    if (!cvarSys.SetInt(L"respawn.InterpolatedRendering", 1)) {
        return std::unexpected<std::string>("Failed to set respawn.InterpolatedRendering");
    }

    m_initialized = true;
    JST_LOG_INFO("Initialized and enabled 'respawn.InterpolatedRendering'.");
    return {};
}

void InterpolatedRenderingTweak::Shutdown() {
    m_initialized = false;
}

} // namespace jst::tweaks
