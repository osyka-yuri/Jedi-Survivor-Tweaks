#pragma once

#include "cvar_resolver.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <span>
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

    /// Batch-resolve several cvar names in a single .rdata/.text scan.
    /// Names not found are silently skipped.
    void ResolveBatch(std::span<const std::wstring_view> names);

    void StartPump(std::chrono::milliseconds period = std::chrono::milliseconds(100));
    void StopPump();

private:
    CVarSystem() = default;
    ~CVarSystem();

    CVarSystem(const CVarSystem&) = delete;
    CVarSystem& operator=(const CVarSystem&) = delete;
    CVarSystem(CVarSystem&&) = delete;
    CVarSystem& operator=(CVarSystem&&) = delete;

    struct PendingEntry {
        std::wstring name;
        std::variant<int32_t, float> value;
        std::chrono::steady_clock::time_point addedAt;
    };

    struct ResolveResult {
        std::optional<ResolvedCVar> cvar;
        std::optional<uintptr_t> pendingPtr;
    };

    template <typename T>
    bool SetTyped(std::wstring_view name, T value);

    template <typename T>
    bool WriteValue(const ResolvedCVar& rc, T value);

    void UpdateFlags(const ResolvedCVar& rc, std::wstring_view name);

    template <typename T>
    bool WriteResolved(const ResolvedCVar& rc, std::wstring_view name, T value);

    [[nodiscard]] ResolveResult Resolve(std::wstring_view name);
    [[nodiscard]] const ModuleInfo* GetOrFetchMod();

    void ProcessPending();
    void PumpLoop(std::chrono::milliseconds period);

    std::mutex m_mutex;
    std::unordered_map<std::wstring, ResolvedCVar, WStringHash, std::equal_to<>> m_cache;
    std::vector<PendingEntry> m_pending;
    std::optional<ModuleInfo> m_mod;
    std::once_flag m_modOnce;

    std::thread m_pumpThread;
    std::condition_variable m_pumpCv;
    bool m_pumpRunning = false;
    bool m_pumpStopRequested = false;
};

} // namespace jst::core
