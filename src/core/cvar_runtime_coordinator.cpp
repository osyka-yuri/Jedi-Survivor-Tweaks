#include "cvar_runtime_coordinator.hpp"

#include "cvar_system.hpp"
#include "game_settings_barrier.hpp"
#include "game_thread_dispatcher.hpp"
#include "hook_engine.hpp"
#include "logging.hpp"

namespace jst::core {

void CVarRuntimeCoordinator::Start(HookEngine& hooks) {
    if (m_started) {
        return;
    }
    m_started = true;

    auto& cvars = CVarSystem::Instance();
    auto& dispatcher = GameThreadDispatcher::Instance();
    auto& settingsBarrier = GameSettingsBarrier::Instance();
    if (auto started = cvars.Start(); !started) {
        Fail(hooks, started.error());
        return;
    }

    auto dispatcherResult = dispatcher.RegisterHook(hooks);
    if (!dispatcherResult) {
        Fail(hooks, dispatcherResult.error());
        return;
    }
    m_dispatcherRegistered = true;
    dispatcher.SetTickHandler([cvarSystem = &cvars] {
        cvarSystem->OnGameThreadTick();
    });

    auto barrierResult = settingsBarrier.RegisterHooks(hooks);
    if (!barrierResult) {
        Fail(hooks, barrierResult.error());
        return;
    }
    m_settingsBarrierRegistered = true;
    m_available = true;
}

void CVarRuntimeCoordinator::FinalizeResolution(HookEngine& hooks) {
    if (!m_available) {
        return;
    }
    auto& dispatcher = GameThreadDispatcher::Instance();
    if (auto result = dispatcher.FinalizeResolution(hooks); !result) {
        Fail(hooks, result.error());
        return;
    }
    if (auto result = GameSettingsBarrier::Instance().FinalizeResolution(hooks);
        !result) {
        Fail(hooks, result.error());
    }
}

void CVarRuntimeCoordinator::FinalizeInstallation(HookEngine& hooks) {
    if (!m_available) {
        return;
    }
    auto& dispatcher = GameThreadDispatcher::Instance();
    if (auto result = dispatcher.FinalizeInstallation(hooks); !result) {
        Fail(hooks, result.error());
        return;
    }
    if (auto result = GameSettingsBarrier::Instance().FinalizeInstallation(hooks);
        !result) {
        Fail(hooks, result.error());
    }
}

void CVarRuntimeCoordinator::BeginShutdown() {
    if (m_started) {
        GameThreadDispatcher::Instance().BeginShutdown();
    }
}

void CVarRuntimeCoordinator::Shutdown(HookEngine& hooks) {
    if (!m_started) {
        return;
    }
    BeginShutdown();
    const std::string rollbackError = RollbackHooks(hooks);
    if (!rollbackError.empty()) {
        JST_LOG_ERROR("CVar service-hook shutdown incomplete: {}", rollbackError);
    }
    CVarSystem::Instance().Stop();
    m_available = false;
    m_started = false;
}

void CVarRuntimeCoordinator::Fail(HookEngine& hooks, std::string reason) {
    const std::string rollbackError = RollbackHooks(hooks);
    if (!rollbackError.empty()) {
        reason += "; rollback: ";
        reason += rollbackError;
    }
    GameThreadDispatcher::Instance().MarkUnavailable(reason);
    GameSettingsBarrier::Instance().MarkUnavailable(reason);
    CVarSystem::Instance().MarkUnavailable(std::move(reason));
    m_available = false;
}

std::string CVarRuntimeCoordinator::RollbackHooks(HookEngine& hooks) {
    std::string firstError;
    auto settingsStopped = GameSettingsBarrier::Instance().ShutdownHooks(hooks);
    if (settingsStopped) {
        m_settingsBarrierRegistered = false;
    } else {
        firstError = settingsStopped.error();
    }
    auto dispatcherStopped = GameThreadDispatcher::Instance().ShutdownHook(hooks);
    if (dispatcherStopped) {
        m_dispatcherRegistered = false;
    } else if (firstError.empty()) {
        firstError = dispatcherStopped.error();
    }
    return firstError;
}

} // namespace jst::core
