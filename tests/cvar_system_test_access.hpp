#pragma once

#if !defined(JST_UNIT_TESTS)
#error "cvar_system_test_access.hpp is test-only"
#endif

#include "core/cvar_system.hpp"

#include <algorithm>
#include <chrono>

namespace jst::core {

class CVarSystemTestAccess final {
public:
    static void Reset(CVarSystem& system) {
        system.Stop();
        CVarSystem::Cache discardedEntries;
        std::vector<std::vector<CVarSystem::IntReadCallback>> discardedCallbacks;
        std::lock_guard lifecycleLock(system.m_lifecycleMutex);
        std::lock_guard lock(system.m_mutex);
        system.FailAllCommandsLocked("reset by test", discardedCallbacks);
        discardedEntries.swap(system.m_cache);
        system.m_managedClaims.clear();
        system.m_needsInitialScan.clear();
        system.m_readyReadBatches.clear();
        system.m_pendingTimeout = std::chrono::milliseconds{30'000};
        system.m_nextGeneration = 1;
        system.m_activeGameThreadPasses = 0;
        system.m_state = CVarSystemState::Running;
        system.m_gameThreadReady = false;
        system.m_startupReconciler.Clear();
        system.m_unavailableReason.clear();
        system.m_module.reset();
        system.m_testPauseResolverAfterModuleFetch = false;
        system.m_testResolverPaused = false;
        system.m_testStopBeforeResolverJoin = false;
        system.m_testResolvedReadDetachmentFailAfter.reset();
        system.m_testLifecycleAttempts = 0;
        system.m_testLifecycleEntries = 0;
    }

    [[nodiscard]] static bool InjectResolved(
        CVarSystem& system,
        std::wstring_view name,
        uintptr_t object,
        uintptr_t setter,
        uintptr_t readAddress = 0,
        [[maybe_unused]] uintptr_t shadowAddress = 0) {
        if (name.empty() || !object || !setter) {
            return false;
        }

        CVarReadLayout layout;
        if (readAddress) {
            layout.kind = CVarStorageKind::ExternalReference;
            layout.referenceOffset = cvar_layout::kRefOffset;
            *reinterpret_cast<uintptr_t*>(object + layout.referenceOffset) =
                readAddress;
        } else {
            layout.kind = CVarStorageKind::Inline;
            layout.valueOffset = cvar_layout::kValueOffset;
        }

        std::lock_guard lock(system.m_mutex);
        auto& entry = system.EnsureEntryLocked(name);
        entry.resolved = ResolvedCVar{
            .cvarObject = object,
            .stringSetter = setter,
            .readLayout = layout,
        };
        std::erase_if(system.m_needsInitialScan, [&](const std::wstring& item) {
            return CVarNameEqual{}(item, name);
        });
        return true;
    }

    [[nodiscard]] static bool CommitPendingAsResolved(
        CVarSystem& system,
        std::wstring_view name,
        uintptr_t object,
        uintptr_t setter,
        uintptr_t readAddress = 0) {
        return InjectResolved(
            system,
            name,
            object,
            setter,
            readAddress);
    }

    [[nodiscard]] static std::optional<std::wstring> PendingValue(
        CVarSystem& system,
        std::wstring_view name) {
        std::lock_guard lock(system.m_mutex);
        const auto found = system.m_cache.find(name);
        if (found == system.m_cache.end() || !found->second.write) {
            return std::nullopt;
        }
        return found->second.write->value;
    }

    static void PumpOnce(CVarSystem& system) {
        system.OnGameThreadTick();
    }

    static void ForceStartupReconciliation(CVarSystem& system) {
        std::lock_guard lock(system.m_mutex);
        (void)system.m_startupReconciler.Start(
            std::chrono::steady_clock::now(),
            std::chrono::hours(1),
            std::chrono::milliseconds::zero());
    }

    static void ExpireStartupReconciliation(CVarSystem& system) {
        std::lock_guard lock(system.m_mutex);
        (void)system.m_startupReconciler.Start(
            std::chrono::steady_clock::now(),
            std::chrono::milliseconds::zero(),
            std::chrono::milliseconds::zero());
    }

    static void ResolveOnce(CVarSystem& system, const ModuleInfo& module) {
        system.ResolvePendingCVars(module);
    }

    static void ScanOnce(CVarSystem& system) {
        system.PerformInitialScan();
    }

    static void SetPendingTimeout(
        CVarSystem& system,
        std::chrono::milliseconds timeout) {
        std::lock_guard lock(system.m_mutex);
        system.m_pendingTimeout = timeout;
    }

    [[nodiscard]] static std::optional<std::chrono::steady_clock::time_point>
    Deadline(CVarSystem& system, std::wstring_view name) {
        std::lock_guard lock(system.m_mutex);
        const auto found = system.m_cache.find(name);
        return found == system.m_cache.end()
            ? std::nullopt
            : found->second.deadline;
    }

    static void ExpireUnresolved(CVarSystem& system) {
        std::vector<CVarSystem::Cache::node_type> detachedEntries;
        std::lock_guard lock(system.m_mutex);
        for (auto& [name, entry] : system.m_cache) {
            (void)name;
            if (!entry.resolved) {
                entry.deadline = std::chrono::steady_clock::now();
            }
        }
        system.SweepExpiredLocked(
            std::chrono::steady_clock::now(), detachedEntries);
    }

    static void SetNextGeneration(CVarSystem& system, uint64_t generation) {
        std::lock_guard lock(system.m_mutex);
        system.m_nextGeneration = generation;
    }

    static void FailResolvedReadDetachmentAfter(
        CVarSystem& system,
        size_t completedDetaches) {
        std::lock_guard lock(system.m_mutex);
        system.m_testResolvedReadDetachmentFailAfter = completedDetaches;
    }

    static void PauseResolverAfterModuleFetch(
        CVarSystem& system,
        ModuleInfo module) {
        std::lock_guard lifecycleLock(system.m_lifecycleMutex);
        std::lock_guard lock(system.m_mutex);
        system.m_module = module;
        system.m_testPauseResolverAfterModuleFetch = true;
        system.m_testResolverPaused = false;
        system.m_testStopBeforeResolverJoin = false;
    }

    static void ResetLifecycleCounters(CVarSystem& system) {
        std::lock_guard lock(system.m_mutex);
        system.m_testLifecycleAttempts = 0;
        system.m_testLifecycleEntries = 0;
    }

    [[nodiscard]] static bool WaitForLifecycleAttempts(
        CVarSystem& system,
        size_t count) {
        std::unique_lock lock(system.m_mutex);
        return system.m_testResolverCv.wait_for(
            lock,
            std::chrono::seconds(1),
            [&system, count] { return system.m_testLifecycleAttempts >= count; });
    }

    [[nodiscard]] static size_t LifecycleEntries(CVarSystem& system) {
        std::lock_guard lock(system.m_mutex);
        return system.m_testLifecycleEntries;
    }

    [[nodiscard]] static bool WaitForResolverPause(CVarSystem& system) {
        std::unique_lock lock(system.m_mutex);
        return system.m_testResolverCv.wait_for(
            lock,
            std::chrono::seconds(1),
            [&system] { return system.m_testResolverPaused; });
    }

    [[nodiscard]] static bool WaitForStopBeforeResolverJoin(CVarSystem& system) {
        std::unique_lock lock(system.m_mutex);
        return system.m_testResolverCv.wait_for(
            lock,
            std::chrono::seconds(1),
            [&system] { return system.m_testStopBeforeResolverJoin; });
    }

    [[nodiscard]] static bool HasCachedModule(CVarSystem& system) {
        std::lock_guard lock(system.m_mutex);
        return system.m_module.has_value();
    }

    static void ResumeResolverAfterModuleFetch(CVarSystem& system) {
        {
            std::lock_guard lock(system.m_mutex);
            system.m_testPauseResolverAfterModuleFetch = false;
        }
        system.m_testResolverCv.notify_all();
    }

    [[nodiscard]] static std::mutex* CacheMutex(CVarSystem& system) {
        return &system.m_mutex;
    }

    static void SetWritesAvailable(CVarSystem& system, bool available) {
        if (!available) {
            system.MarkUnavailable("disabled by test");
        }
    }

    [[nodiscard]] static size_t CacheSize(CVarSystem& system) {
        std::lock_guard lock(system.m_mutex);
        return system.m_cache.size();
    }

    [[nodiscard]] static size_t InitialScanQueueSize(CVarSystem& system) {
        std::lock_guard lock(system.m_mutex);
        return system.m_needsInitialScan.size();
    }

};

} // namespace jst::core
