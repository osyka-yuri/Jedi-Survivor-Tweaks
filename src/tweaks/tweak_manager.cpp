#include "tweak_manager.hpp"
#include "core/config.hpp"
#include "core/hook_engine.hpp"
#include "core/logging.hpp"

#include <format>

namespace jst::tweaks {

std::expected<size_t, std::string> TweakManager::Initialize(core::HookEngine& hooks, core::Config& config) {
    if (m_initialized) return 0;

    // Step 1: register-only initialization. For hook tweaks this just hands
    // patterns to the engine without scanning .text. CVar-only tweaks fully
    // initialize here.
    std::vector<ITweak*> activeTweaks;
    activeTweaks.reserve(m_tweaks.size());
    for (auto& tweak : m_tweaks) {
        const std::string_view tweakName = tweak->Name();
        const bool enabled = config.GetBool(tweakName, "Enabled", tweak->IsEnabledByDefault());
        if (!enabled) {
            JST_LOG_INFO("Tweak '{}' is disabled in config.", tweakName);
            continue;
        }
        JST_LOG_INFO("Initializing tweak: '{}'.", tweakName);

        auto initRes = tweak->Initialize(hooks, config);
        if (!initRes) {
            JST_LOG_ERROR("Failed to initialize tweak: '{}'. Error: '{}'.", tweakName, initRes.error());
            continue;
        }
        activeTweaks.push_back(tweak.get());
    }

    // Step 2: single-pass batch scan resolves every pending pattern hook.
    for (const auto& error : hooks.ResolveAll()) {
        JST_LOG_ERROR("Hook resolve failure [{}]: {}", error.site, error.message);
    }

    // Step 3: hand continuation addresses to each still-active tweak. A
    // multi-site tweak unregisters its entire group if any binding is missing.
    std::vector<ITweak*> resolvedTweaks;
    resolvedTweaks.reserve(activeTweaks.size());
    for (auto* tweak : activeTweaks) {
        auto finRes = tweak->FinalizeResolution(hooks);
        if (!finRes) {
            JST_LOG_ERROR("Failed to finalize tweak: '{}'. Error: '{}'.",
                          tweak->Name(), finRes.error());
            continue;
        }
        resolvedTweaks.push_back(tweak);
    }

    // Step 4: prepare every gateway, seal the arena, then install each public
    // hook group transactionally while other process threads are suspended.
    for (const auto& error : hooks.InstallAll()) {
        JST_LOG_ERROR("Hook install failure [{}]: {}", error.site, error.message);
    }

    // Step 5: publish only fully installed groups to the overlay/runtime state.
    size_t enabledCount = 0;
    for (auto* tweak : resolvedTweaks) {
        auto installed = tweak->FinalizeInstallation(hooks);
        if (!installed) {
            JST_LOG_ERROR("Failed to publish tweak '{}'. Error: '{}'.",
                          tweak->Name(), installed.error());
            continue;
        }
        ++enabledCount;
    }

    m_initialized = true;
    JST_LOG_INFO("Initialized. Active tweaks: {}/{}.",
                 enabledCount, m_tweaks.size());
    return enabledCount;
}

void TweakManager::IterateTweaks(std::function<void(ITweak&)> visitor) const {
    for (const auto& tweak : m_tweaks) {
        visitor(*tweak);
    }
}

void TweakManager::Shutdown() {
    if (!m_initialized) return;

    for (auto it = m_tweaks.rbegin(); it != m_tweaks.rend(); ++it) {
        if ((*it)->IsInitialized()) {
            (*it)->Shutdown();
        }
    }
    m_tweaks.clear();
    m_initialized = false;
}

} // namespace jst::tweaks
