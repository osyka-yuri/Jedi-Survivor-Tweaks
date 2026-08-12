#include "core/cvar_runtime_coordinator.hpp"
#include "core/cvar_system.hpp"
#include "core/game_thread_dispatcher.hpp"
#include "core/hook_engine.hpp"
#include "cvar_runtime_coordinator_test_access.hpp"
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

constexpr std::string_view kServiceHookName = "Core.FEngineLoop.Tick";

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

void RegisterRollbackSentinel(jst::core::HookEngine& hooks) {
    auto registered = hooks.RegisterAddressHook(
        jst::core::HookSiteSpec{
            .name = std::string(kServiceHookName),
            .group = "Coordinator.RollbackTest",
            .minimumOverwriteLength = 5,
            .continuation = jst::core::HookContinuation::Resume,
        },
        ModuleRva(kRollbackTarget.data()),
        reinterpret_cast<uintptr_t>(&CoordinatorTestDetour));
    Check(registered.has_value(),
          "rollback sentinel hook registers through HookEngine");
    Check(hooks.GetContinuationAddress(kServiceHookName).has_value(),
          "rollback sentinel is present before lifecycle failure");
}

bool HookWasRemoved(const jst::core::HookEngine& hooks) {
    return !hooks.GetContinuationAddress(kServiceHookName).has_value();
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
        RegisterRollbackSentinel(hooks);
        CVarRuntimeCoordinator coordinator;
        GameThreadDispatcherTestAccess::Reset(
            GameThreadDispatcher::Instance(),
            nullptr,
            GameThreadDispatcherState::WaitingForFirstTick);
        CVarRuntimeCoordinatorTestAccess::PublishRegisteredHooks(
            coordinator,
            true);
        coordinator.FinalizeResolution(hooks);

        Check(HookWasRemoved(hooks) &&
                  !coordinator.IsAvailable() &&
                  !CVarRuntimeCoordinatorTestAccess::HasRegisteredHooks(
                      coordinator),
              "dispatcher resolution failure rolls back its service hook");
        Check(CVarSystem::Instance().State() == CVarSystemState::Unavailable &&
                  GameThreadDispatcher::Instance().State() ==
                      GameThreadDispatcherState::Unavailable,
              "resolution failure marks the CVar services unavailable");

        coordinator.Shutdown(hooks);
        coordinator.Shutdown(hooks);
        Check(CVarSystem::Instance().State() == CVarSystemState::Stopped &&
                  GameThreadDispatcher::Instance().State() ==
                      GameThreadDispatcherState::Stopped,
              "CVar lifecycle shutdown is idempotent after rollback");
    }

    {
        HookEngine hooks;
        RegisterRollbackSentinel(hooks);
        CVarRuntimeCoordinator coordinator;
        GameThreadDispatcherTestAccess::Reset(
            GameThreadDispatcher::Instance(),
            nullptr,
            GameThreadDispatcherState::Resolving);
        CVarRuntimeCoordinatorTestAccess::PublishRegisteredHooks(
            coordinator,
            true);
        coordinator.FinalizeResolution(hooks);
        Check(coordinator.IsAvailable(),
              "all real service continuations bind before installation");
        coordinator.FinalizeInstallation(hooks);
        Check(HookWasRemoved(hooks) &&
                  !coordinator.IsAvailable() &&
                  !CVarRuntimeCoordinatorTestAccess::HasRegisteredHooks(
                      coordinator),
              "dispatcher installation failure rolls back its service hook");
        coordinator.Shutdown(hooks);
    }
}
