#pragma once

#include "cvar_resolver.hpp"
#include "cvar_overrides.hpp"
#include "cvar_scanner.hpp"

#include "cvar_layout.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace jst::core {

/**
 * Thread-safe resolver and writer for Unreal Engine console variables.
 *
 * Resolution strategy:
 *   1. Scan .rdata for the CVar name string.
 *   2. Scan .text for references to that string.
 *   3. Derive either a global pointer (preferred) or a direct reference variable.
 *   4. Cache the resolution and write the value.
 *
 * If the CVar object isn't constructed yet, the write is queued and retried
 * by a background pump thread until success or timeout.
 */
class CVarSystem final {
public:
    [[nodiscard]] static CVarSystem& Instance();

    bool SetInt(std::wstring_view name, int32_t value);
    bool SetFloat(std::wstring_view name, float value);

    void StartPump(std::chrono::milliseconds period = std::chrono::milliseconds(100));
    void StopPump();

private:
    CVarSystem() = default;
    ~CVarSystem();

    CVarSystem(const CVarSystem&) = delete;
    CVarSystem& operator=(const CVarSystem&) = delete;
    CVarSystem(CVarSystem&&) = delete;
    CVarSystem& operator=(CVarSystem&&) = delete;

    // A CVar can be in one of three internal states:
    // 1. Unknown:  No entry in m_cache — CVar has never been requested.
    // 2. Pending:  Entry exists, but resolution hasn't succeeded yet.
    //              Sub-state a (awaiting scan):   scanData.strAddr == 0
    //              Sub-state b (awaiting object): scanData.strAddr != 0, but
    //                          the CVar heap object is still null (late init).
    // 3. Resolved: Object is live; write targets cached in ResolvedState.
    using CVarValue = std::variant<int32_t, float>;

    struct PendingState {
        ScanEntry scanData;
        CVarValue targetValue;
        std::chrono::steady_clock::time_point firstSeen;
    };

    struct ResolvedState {
        ResolvedCVar rc;
    };

    struct CVarState {
        std::variant<std::monostate, PendingState, ResolvedState> state;
    };

    template <typename T>
    bool SetTyped(std::wstring_view name, T value);

    template <typename T>
    bool WriteValue(const ResolvedCVar& rc, T value);

    void UpdateFlags(const ResolvedCVar& rc, std::wstring_view name);

    template <typename T>
    bool WriteResolved(const ResolvedCVar& rc, std::wstring_view name, T value);

    [[nodiscard]] const ModuleInfo* GetOrFetchMod();

    void PerformInitialScan();
    void PumpLoop(std::chrono::milliseconds period);

    std::mutex m_mutex;
    // Maps a CVar name to its current state.
    std::unordered_map<std::wstring, CVarState, WStringHash, std::equal_to<>> m_cache;
    // Names of CVars that were just added and need an initial .rdata/.text scan.
    std::vector<std::wstring> m_needsInitialScan;
    // Number of CVars currently in PendingState (protected by m_mutex).
    // Used by PumpLoop to distinguish "idle — wait indefinitely" from
    // "pending retries — wake every period".
    size_t m_pendingCount = 0;

    std::optional<ModuleInfo> m_mod;
    std::once_flag m_modOnce;

    std::thread m_pumpThread;
    std::condition_variable m_pumpCv;
    bool m_pumpRunning = false;
    bool m_pumpStopRequested = false;
};

} // namespace jst::core
