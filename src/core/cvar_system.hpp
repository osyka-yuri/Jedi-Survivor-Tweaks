#pragma once

#include "cvar_overrides.hpp"
#include "cvar_resolver.hpp"
#include "cvar_scanner.hpp"
#include "cvar_watch.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace jst::core {

class CVarWatchRegistry;

#if defined(JST_UNIT_TESTS)
class CVarSystemTestAccess;
#endif

/**
 * Thread-safe resolver, writer and pump for Unreal Engine console variables.
 * Unknown names are scanned once, late-constructed objects are retried, and
 * integer observations share the same pump through RAII subscriptions.
 */
class CVarSystem final {
public:
    [[nodiscard]] static CVarSystem& Instance();

    bool SetInt(std::wstring_view name, int32_t value);
    bool SetFloat(std::wstring_view name, float value);
    [[nodiscard]] CVarWatchSubscription WatchInt(IntWatchRequest request);

    void StartPump(std::chrono::milliseconds period = std::chrono::milliseconds(100));
    void StopPump();

private:
    friend class CVarWatchSubscription;

    CVarSystem();
    ~CVarSystem();

    CVarSystem(const CVarSystem&) = delete;
    CVarSystem& operator=(const CVarSystem&) = delete;
    CVarSystem(CVarSystem&&) = delete;
    CVarSystem& operator=(CVarSystem&&) = delete;

    using CVarValue = std::variant<int32_t, float>;

    struct PendingState {
        ScanEntry scanData;
        // nullopt means resolve for observers without an outstanding write.
        std::optional<CVarValue> targetValue;
        std::chrono::steady_clock::time_point firstSeen;
    };

    struct ResolvedEntry {
        explicit ResolvedEntry(ResolvedCVar value) : resolved(std::move(value)) {}

        ResolvedCVar resolved;
        mutable std::mutex valueMutex;
        std::optional<CVarValue> pendingWrite;
        std::chrono::steady_clock::time_point pendingWriteSince{};
    };

    using ResolvedEntryPtr = std::shared_ptr<ResolvedEntry>;
    using CVarState = std::variant<PendingState, ResolvedEntryPtr>;

    template <typename T>
    bool SetTyped(std::wstring_view name, T value);
    template <typename T>
    bool WriteValue(const ResolvedCVar& resolved, T value);
    template <typename T>
    bool WriteResolved(const ResolvedCVar& resolved, std::wstring_view name, T value);

    void UpdateFlags(const ResolvedCVar& resolved, std::wstring_view name);
    void EnqueuePendingLocked(std::wstring_view name, std::optional<CVarValue> target);
    [[nodiscard]] bool HasPendingLocked() const;
    [[nodiscard]] std::optional<int32_t> ReadResolvedInt(std::wstring_view name) const;
    void RetryPendingWrites();

    [[nodiscard]] const ModuleInfo* GetOrFetchModule();
    void PerformInitialScan();
    void ResolvePendingCVars(const ModuleInfo& module);
    bool CommitResolved(std::wstring_view name, const ResolvedCVar& resolved);
    void PumpLoop(std::chrono::milliseconds period);

    void CancelWatch(uint64_t id);

    mutable std::mutex m_mutex;
    std::unordered_map<std::wstring, CVarState, WStringHash, std::equal_to<>> m_cache;
    std::vector<std::wstring> m_needsInitialScan;
    std::chrono::milliseconds m_pendingTimeout{30'000};

    mutable std::mutex m_moduleMutex;
    std::optional<ModuleInfo> m_module;

    std::unique_ptr<CVarWatchRegistry> m_watches;
    std::thread m_pumpThread;
    std::condition_variable m_pumpCv;
    bool m_pumpRunning = false;
    bool m_pumpStopRequested = false;

#if defined(JST_UNIT_TESTS)
    friend class CVarSystemTestAccess;
#endif
};

} // namespace jst::core
