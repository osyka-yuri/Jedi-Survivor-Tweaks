#include "tweak_manager.hpp"

#include "core/config.hpp"
#include "core/hook_engine.hpp"

namespace jst::tweaks {

std::expected<void, std::string> TweakManager::Prepare(
    core::HookEngine& hooks,
    const core::Config& config) {
    if (m_started) {
        return std::unexpected("TweakManager has already started");
    }
    m_started = true;

    for (auto& entry : m_entries) {
        auto configured = entry.tweak->Configure(config);
        if (!configured) {
            entry.status = TweakStatus{
                .stage = TweakStage::Failed,
                .error = configured.error(),
            };
            JST_LOG_ERROR(
                "Failed to configure tweak '{}': {}.",
                entry.tweak->Name(),
                configured.error());
            continue;
        }

        if (!configured->enabled &&
            entry.tweak->ActivationMode() ==
                TweakActivationMode::LaunchGated) {
            entry.status.stage = TweakStage::Disabled;
            JST_LOG_INFO(
                "Tweak '{}' is disabled in config.",
                entry.tweak->Name());
            continue;
        }

        auto prepared = entry.tweak->Prepare(hooks);
        if (!prepared) {
            entry.status = TweakStatus{
                .stage = TweakStage::Failed,
                .error = prepared.error(),
            };
            JST_LOG_ERROR(
                "Failed to prepare tweak '{}': {}.",
                entry.tweak->Name(),
                prepared.error());
            continue;
        }
        entry.prepared = true;
        entry.status.stage = TweakStage::Prepared;
    }
    return {};
}

void TweakManager::FinalizeResolution(core::HookEngine& hooks) {
    for (auto& entry : m_entries) {
        if (entry.status.stage != TweakStage::Prepared) {
            continue;
        }
        auto finalized = entry.tweak->FinalizeResolution(hooks);
        if (!finalized) {
            entry.status = TweakStatus{
                .stage = TweakStage::Failed,
                .error = finalized.error(),
            };
            JST_LOG_ERROR(
                "Failed to finalize tweak resolution '{}': {}.",
                entry.tweak->Name(),
                finalized.error());
            continue;
        }
        entry.status.stage = TweakStage::Resolved;
    }
}

size_t TweakManager::FinalizeInstallation(core::HookEngine& hooks) {
    size_t activeCount = 0;
    for (auto& entry : m_entries) {
        if (entry.status.stage != TweakStage::Resolved) {
            continue;
        }
        auto finalized = entry.tweak->FinalizeInstallation(hooks);
        if (!finalized) {
            entry.status = TweakStatus{
                .stage = TweakStage::Failed,
                .error = finalized.error(),
            };
            JST_LOG_ERROR(
                "Failed to publish tweak '{}': {}.",
                entry.tweak->Name(),
                finalized.error());
            continue;
        }
        entry.status.stage = TweakStage::Active;
        ++activeCount;
    }

    JST_LOG_INFO(
        "Initialized. Active tweaks: {}/{}.",
        activeCount,
        m_entries.size());
    return activeCount;
}

void TweakManager::IterateTweaks(
    const std::function<void(ITweak&, const TweakStatus&)>& visitor) {
    for (auto& entry : m_entries) {
        visitor(*entry.tweak, entry.status);
    }
}

void TweakManager::IterateTweaks(
    const std::function<void(const ITweak&, const TweakStatus&)>& visitor)
    const {
    for (const auto& entry : m_entries) {
        visitor(*entry.tweak, entry.status);
    }
}

void TweakManager::Shutdown() {
    if (!m_started) {
        return;
    }
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
        if (it->prepared) {
            it->tweak->Shutdown();
            it->prepared = false;
        }
    }
    m_entries.clear();
    m_started = false;
}

} // namespace jst::tweaks
