#pragma once

#if !defined(JST_UNIT_TESTS)
#error "cvar_runtime_coordinator_test_access.hpp is test-only"
#endif

#include "core/cvar_runtime_coordinator.hpp"
#include "core/cvar_system.hpp"

namespace jst::core {

class CVarRuntimeCoordinatorTestAccess final {
public:
    static void PublishRegisteredHooks(
        CVarRuntimeCoordinator& coordinator,
        bool dispatcherRegistered,
        bool settingsBarrierRegistered) {
        (void)CVarSystem::Instance().Start();
        coordinator.m_started = true;
        coordinator.m_available = true;
        coordinator.m_dispatcherRegistered = dispatcherRegistered;
        coordinator.m_settingsBarrierRegistered = settingsBarrierRegistered;
    }

    [[nodiscard]] static bool HasRegisteredHooks(
        const CVarRuntimeCoordinator& coordinator) {
        return coordinator.m_dispatcherRegistered ||
            coordinator.m_settingsBarrierRegistered;
    }
};

} // namespace jst::core
