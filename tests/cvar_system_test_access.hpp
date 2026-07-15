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
        system.StopPump();
        {
            std::lock_guard lock(system.m_mutex);
            system.m_cache.clear();
            system.m_needsInitialScan.clear();
            system.m_pendingTimeout = std::chrono::milliseconds{30'000};
            system.m_pumpStopRequested = false;
        }
        {
            std::lock_guard lock(system.m_moduleMutex);
            system.m_module.reset();
        }
    }

    [[nodiscard]] static bool InjectResolvedInt(
        CVarSystem& system, std::wstring_view name, int32_t* storage) {
        if (name.empty() || !storage) {
            return false;
        }

        ResolvedCVar resolved;
        resolved.writeAddr = reinterpret_cast<uintptr_t>(storage);
        auto entry = std::make_shared<CVarSystem::ResolvedEntry>(resolved);

        std::lock_guard lock(system.m_mutex);
        system.m_cache.insert_or_assign(std::wstring(name), std::move(entry));
        std::erase(system.m_needsInitialScan, std::wstring(name));
        system.m_pumpCv.notify_all();
        return true;
    }

    [[nodiscard]] static bool CommitPendingAsResolvedInt(
        CVarSystem& system,
        std::wstring_view name,
        int32_t* storage,
        uintptr_t shadowAddress = 0) {
        if (name.empty() || !storage) {
            return false;
        }
        ResolvedCVar resolved;
        resolved.writeAddr = reinterpret_cast<uintptr_t>(storage);
        resolved.writeAddrShadow = shadowAddress;
        return system.CommitResolved(name, resolved);
    }

    static bool SetResolvedShadowAddress(
        CVarSystem& system, std::wstring_view name, uintptr_t shadowAddress) {
        CVarSystem::ResolvedEntryPtr entry;
        {
            std::lock_guard lock(system.m_mutex);
            const auto found = system.m_cache.find(name);
            if (found == system.m_cache.end()) {
                return false;
            }
            const auto* resolved = std::get_if<CVarSystem::ResolvedEntryPtr>(&found->second);
            if (!resolved) {
                return false;
            }
            entry = *resolved;
        }
        std::lock_guard valueLock(entry->valueMutex);
        entry->resolved.writeAddrShadow = shadowAddress;
        return true;
    }

    [[nodiscard]] static bool HasPendingWrite(
        CVarSystem& system, std::wstring_view name) {
        CVarSystem::ResolvedEntryPtr entry;
        {
            std::lock_guard lock(system.m_mutex);
            const auto found = system.m_cache.find(name);
            if (found == system.m_cache.end()) {
                return false;
            }
            const auto* resolved = std::get_if<CVarSystem::ResolvedEntryPtr>(&found->second);
            if (!resolved) {
                return false;
            }
            entry = *resolved;
        }
        std::lock_guard valueLock(entry->valueMutex);
        return entry->pendingWrite.has_value();
    }

    static void PumpOnce(CVarSystem& system) {
        system.PerformInitialScan();
        system.RetryPendingWrites();
        if (const auto* module = system.GetOrFetchModule(); module && module->base != 0) {
            system.ResolvePendingCVars(*module);
        }
        system.m_watches->Evaluate([&system](std::wstring_view name) {
            return system.ReadResolvedInt(name);
        });
    }

    static void SetPendingTimeout(
        CVarSystem& system, std::chrono::milliseconds timeout) {
        std::lock_guard lock(system.m_mutex);
        system.m_pendingTimeout = timeout;
    }

    static void SimulateModuleUnavailable(CVarSystem& system) {
        std::lock_guard lock(system.m_moduleMutex);
        system.m_module = ModuleInfo{};
    }

    [[nodiscard]] static size_t InitialScanQueueSize(CVarSystem& system) {
        std::lock_guard lock(system.m_mutex);
        return system.m_needsInitialScan.size();
    }

    [[nodiscard]] static bool InjectAgedUnresolvedPending(
        CVarSystem& system,
        std::wstring_view name,
        std::optional<int32_t> targetValue) {
        if (name.empty()) {
            return false;
        }

        CVarSystem::PendingState pending;
        pending.targetValue = targetValue
            ? std::optional<CVarSystem::CVarValue>{*targetValue}
            : std::nullopt;
        pending.firstSeen = std::chrono::steady_clock::now() - std::chrono::hours{1};
        pending.scanData.name = std::wstring(name);
        pending.scanData.strAddr = 1;

        std::lock_guard lock(system.m_mutex);
        system.m_cache.insert_or_assign(std::wstring(name), std::move(pending));
        system.m_pumpCv.notify_all();
        return true;
    }

    [[nodiscard]] static bool HasCacheEntry(
        CVarSystem& system, std::wstring_view name) {
        std::lock_guard lock(system.m_mutex);
        return system.m_cache.contains(name);
    }
};

} // namespace jst::core
