#include "tweak_manager.hpp"
#include "core/config.hpp"
#include "core/hook_engine.hpp"
#include "core/logging.hpp"

#include <format>

namespace jst::tweaks {

namespace {
    std::string JoinErrors(const std::vector<std::string>& errs) {
        std::string out;
        for (size_t i = 0; i < errs.size(); ++i) {
            if (i != 0) out += "; ";
            out += errs[i];
        }
        return out;
    }
}

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
    auto resolveRes = hooks.ResolveAll();
    if (!resolveRes) {
        // Log all resolve errors but keep going -- hooks that did resolve can
        // still install, and TweakManager mirrors the existing best-effort
        // semantics. FinalizeResolution per tweak will catch unresolved cases.
        for (const auto& e : resolveRes.error()) {
            JST_LOG_ERROR("Hook resolve failure: {}", e);
        }
    }

    // Step 3: hand resume addresses to each (still-active) tweak.
    size_t enabledCount = 0;
    for (auto* tweak : activeTweaks) {
        auto finRes = tweak->FinalizeResolution(hooks);
        if (!finRes) {
            JST_LOG_ERROR("Failed to finalize tweak: '{}'. Error: '{}'.",
                          tweak->Name(), finRes.error());
            continue;
        }
        ++enabledCount;
    }

    // Step 4: write all detours under a single ThreadSuspender pass.
    auto installRes = hooks.InstallAll();
    if (!installRes) {
        const auto& errs = installRes.error();
        return std::unexpected(std::format("Failed to install {} hook(s): {}",
                                           errs.size(), JoinErrors(errs)));
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
