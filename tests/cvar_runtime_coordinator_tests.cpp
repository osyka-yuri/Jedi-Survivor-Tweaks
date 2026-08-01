#include "core/cvar_runtime_coordinator.hpp"
#include "core/cvar_system.hpp"
#include "core/game_settings_barrier.hpp"
#include "core/game_thread_dispatcher.hpp"
#include "core/hook_engine.hpp"
#include "cvar_runtime_coordinator_test_access.hpp"
#include "game_settings_barrier_test_access.hpp"
#include "game_thread_dispatcher_test_access.hpp"
#include "test_check.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace {

constexpr std::array<std::string_view, 4> kServiceHookNames{
    "Core.FEngineLoop.Tick",
    "Core.GameSettingsBarrier.SetString",
    "Core.GameSettingsBarrier.SetFloat",
    "Core.GameSettingsBarrier.SetInt",
};

#pragma section(".coordinatortest", read, execute)
__declspec(allocate(".coordinatortest")) alignas(16)
const std::array<std::byte, 16> kRollbackTarget{
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0xC3},
};

extern "C" __declspec(noinline) void CoordinatorTestDetour() {}

uintptr_t ModuleRva(const void* address) {
    return reinterpret_cast<uintptr_t>(address) -
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

void RegisterRollbackSentinels(
    jst::core::HookEngine& hooks,
    size_t count) {
    for (const auto name : std::span(kServiceHookNames).first(count)) {
        auto registered = hooks.RegisterAddressHook(
            jst::core::HookSiteSpec{
                .name = std::string(name),
                .group = "Coordinator.RollbackTest",
                .minimumOverwriteLength = 5,
                .continuation = jst::core::HookContinuation::Resume,
            },
            ModuleRva(kRollbackTarget.data()),
            reinterpret_cast<uintptr_t>(&CoordinatorTestDetour));
        Check(registered.has_value(),
              "rollback sentinel hook registers through HookEngine");
        Check(hooks.GetContinuationAddress(name).has_value(),
              "rollback sentinel is present before lifecycle failure");
    }
}

bool HooksWereRemoved(
    const jst::core::HookEngine& hooks,
    size_t count) {
    for (const auto name : std::span(kServiceHookNames).first(count)) {
        if (hooks.GetContinuationAddress(name).has_value()) {
            return false;
        }
    }
    return true;
}

} // namespace

void TestCVarRuntimeCoordinator() {
    using namespace jst::core;

    const auto invalidStart = CVarSystem::Instance().Start(
        std::chrono::milliseconds::zero());
    Check(!invalidStart &&
              CVarSystem::Instance().State() == CVarSystemState::Stopped,
          "invalid resolver startup fails without publishing partial state");

    {
        HookEngine hooks;
        CVarRuntimeCoordinator coordinator;
        coordinator.Start(hooks);
        Check(!coordinator.IsAvailable() &&
                  !CVarRuntimeCoordinatorTestAccess::HasRegisteredHooks(
                      coordinator),
              "real dispatcher registration failure leaves no published service hooks");
        coordinator.Shutdown(hooks);
    }

    {
        HookEngine hooks;
        RegisterRollbackSentinels(hooks, 3);
        CVarRuntimeCoordinator coordinator;
        GameThreadDispatcherTestAccess::Reset(
            GameThreadDispatcher::Instance(),
            nullptr,
            GameThreadDispatcherState::Resolving);
        GameSettingsBarrierTestAccess::Reset(
            GameSettingsBarrier::Instance(),
            GameSettingsBarrierState::Resolving);
        CVarRuntimeCoordinatorTestAccess::PublishRegisteredHooks(
            coordinator,
            true,
            true);
        coordinator.FinalizeResolution(hooks);

        Check(HooksWereRemoved(hooks, 3) &&
                  !coordinator.IsAvailable() &&
                  !CVarRuntimeCoordinatorTestAccess::HasRegisteredHooks(
                      coordinator),
              "real continuation-resolution failure rolls back both service-hook groups");
        Check(CVarSystem::Instance().State() == CVarSystemState::Unavailable &&
                  GameThreadDispatcher::Instance().State() ==
                      GameThreadDispatcherState::Unavailable &&
                  GameSettingsBarrier::Instance().State() ==
                      GameSettingsBarrierState::Unavailable,
              "resolution failure marks every dependent service unavailable");

        coordinator.Shutdown(hooks);
        coordinator.Shutdown(hooks);
        Check(CVarSystem::Instance().State() == CVarSystemState::Stopped &&
                  GameThreadDispatcher::Instance().State() ==
                      GameThreadDispatcherState::Stopped &&
                  GameSettingsBarrier::Instance().State() ==
                      GameSettingsBarrierState::Stopped,
              "CVar lifecycle shutdown is idempotent after rollback");
    }

    {
        HookEngine hooks;
        RegisterRollbackSentinels(hooks, kServiceHookNames.size());
        CVarRuntimeCoordinator coordinator;
        GameThreadDispatcherTestAccess::Reset(
            GameThreadDispatcher::Instance(),
            nullptr,
            GameThreadDispatcherState::Resolving);
        GameSettingsBarrierTestAccess::Reset(
            GameSettingsBarrier::Instance(),
            GameSettingsBarrierState::Resolving);
        CVarRuntimeCoordinatorTestAccess::PublishRegisteredHooks(
            coordinator,
            true,
            true);
        coordinator.FinalizeResolution(hooks);
        Check(coordinator.IsAvailable(),
              "all real service continuations bind before installation");
        coordinator.FinalizeInstallation(hooks);
        Check(HooksWereRemoved(hooks, kServiceHookNames.size()) &&
                  !coordinator.IsAvailable() &&
                  !CVarRuntimeCoordinatorTestAccess::HasRegisteredHooks(
                      coordinator),
              "real installation-state failure rolls back both service-hook groups");
        coordinator.Shutdown(hooks);
    }
}
