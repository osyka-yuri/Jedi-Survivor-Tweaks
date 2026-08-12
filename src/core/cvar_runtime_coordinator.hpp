#pragma once

#include <string>

namespace jst::core {

class HookEngine;
#if defined(JST_UNIT_TESTS)
class CVarRuntimeCoordinatorTestAccess;
#endif

// Owns the game-thread dispatcher required by CVarSystem. Any phase failure
// removes that service hook and fails CVar access closed without affecting
// independent tweak hooks.
class CVarRuntimeCoordinator final {
public:
    void Start(HookEngine& hooks);
    void FinalizeResolution(HookEngine& hooks);
    void FinalizeInstallation(HookEngine& hooks);
    void BeginShutdown();
    void Shutdown(HookEngine& hooks);

    [[nodiscard]] bool IsAvailable() const noexcept { return m_available; }

private:
    void Fail(HookEngine& hooks, std::string reason);
    [[nodiscard]] std::string RollbackHooks(HookEngine& hooks);

    bool m_started = false;
    bool m_available = false;
    bool m_dispatcherRegistered = false;

#if defined(JST_UNIT_TESTS)
    friend class CVarRuntimeCoordinatorTestAccess;
#endif
};

} // namespace jst::core
