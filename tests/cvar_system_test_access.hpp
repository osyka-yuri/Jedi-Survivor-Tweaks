#pragma once

#if !defined(JST_UNIT_TESTS)
#error "cvar_system_test_access.hpp is test-only"
#endif

#include "core/cvar_system.hpp"
#include "core/cvar_watch_registry.hpp"

#include <algorithm>

namespace jst::core {

class CVarSystemTestAccess final {
public:
    static void Reset(CVarSystem& system) {
        system.Stop();
        std::lock_guard lock(system.m_mutex);
        system.m_cache.clear();
        system.m_managedClaims.clear();
        system.m_needsInitialScan.clear();
        system.m_pendingTimeout = std::chrono::milliseconds{30'000};
        system.m_nextGeneration = 1;
        system.m_activeGameThreadPasses = 0;
        system.m_state = CVarSystemState::Running;
        system.m_gameThreadReady = false;
        system.m_gameSettingsReady = true;
        system.m_watchBarrierOpened = false;
        system.m_unavailableReason.clear();
        system.m_module.reset();
        system.m_nextWatchEvaluation = {};
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
        system.m_nextWatchEvaluation = {};
        system.OnGameThreadTick();
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

    static void SetWritesAvailable(CVarSystem& system, bool available) {
        if (!available) {
            system.MarkUnavailable("disabled by test");
        }
    }

    static void SetGameSettingsReady(CVarSystem& system, bool ready) {
        std::lock_guard lock(system.m_mutex);
        system.m_gameSettingsReady = ready;
        if (!ready) {
            system.m_watchBarrierOpened = false;
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

    [[nodiscard]] static bool HasWatchFor(
        CVarSystem& system,
        std::wstring_view name) {
        return system.m_watches->HasFor(name);
    }

    [[nodiscard]] static size_t WatchEntryCount(CVarSystem& system) {
        return system.m_watches->EntryCount();
    }

};

} // namespace jst::core
