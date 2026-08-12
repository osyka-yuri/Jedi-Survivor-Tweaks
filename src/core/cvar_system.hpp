#pragma once

#include "cvar_name.hpp"
#include "cvar_resolver.hpp"
#include "cvar_scanner.hpp"
#include "cvar_startup_reconciler.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace jst::core {

class CVarSystem;

#if defined(JST_UNIT_TESTS)
class CVarSystemTestAccess;
#endif

enum class CVarSetResult : uint8_t {
    Queued,
    Replaced,
    Rejected,
};

enum class CVarCommandSource : uint8_t {
    Custom,
    Managed,
};

enum class CVarRejectionReason : uint8_t {
    None,
    InvalidRequest,
    SystemUnavailable,
    ManagedConflict,
};

enum class CVarCommandState : uint8_t {
    Pending,
    Applied,
    Superseded,
    Failed,
};

enum class CVarSupersedeReason : uint8_t {
    None,
    NewerValue,
    ManagedPriority,
};

struct CVarCommandSnapshot {
    CVarCommandState state = CVarCommandState::Failed;
    CVarSupersedeReason supersedeReason = CVarSupersedeReason::None;
    std::string diagnostic;
};

class CVarCommandTicket final {
public:
    CVarCommandTicket() = default;

    [[nodiscard]] CVarCommandSnapshot Snapshot() const;
    [[nodiscard]] explicit operator bool() const noexcept {
        return m_state != nullptr;
    }

private:
    struct SharedState;
    explicit CVarCommandTicket(std::shared_ptr<SharedState> state)
        : m_state(std::move(state)) {}

    std::shared_ptr<SharedState> m_state;

    friend class CVarSystem;
};

struct CVarQueueResult {
    CVarSetResult result = CVarSetResult::Rejected;
    CVarCommandTicket ticket;
    CVarRejectionReason rejection = CVarRejectionReason::None;

    [[nodiscard]] bool Accepted() const noexcept {
        return result != CVarSetResult::Rejected;
    }
};

struct CVarWriteRequest {
    std::wstring name;
    // The public request is typed. Text and the comparison representation are
    // derived once by CVarSystem, before any cache state is mutated.
    std::variant<std::wstring, int32_t, float> value;
};

struct CVarBatchResult {
    std::vector<CVarQueueResult> commands;
    CVarRejectionReason rejection = CVarRejectionReason::None;
    std::string diagnostic;

    [[nodiscard]] bool Accepted() const noexcept {
        return rejection == CVarRejectionReason::None;
    }
};

enum class CVarSystemState : uint8_t {
    Stopped,
    Running,
    Unavailable,
    Stopping,
};

/**
 * Resolves CVar objects away from the game thread. Engine reads and setter
 * commands execute after FEngineLoop::Tick returns.
 */
class CVarSystem final {
public:
    using IntReadCallback =
        std::function<void(std::expected<int32_t, std::string>)>;

    [[nodiscard]] static CVarSystem& Instance();

    [[nodiscard]] CVarQueueResult SetString(
        std::wstring_view name,
        std::wstring_view value,
        CVarCommandSource source = CVarCommandSource::Managed);
    [[nodiscard]] CVarQueueResult SetInt(
        std::wstring_view name,
        int32_t value,
        CVarCommandSource source = CVarCommandSource::Managed);
    [[nodiscard]] CVarQueueResult SetFloat(
        std::wstring_view name,
        float value,
        CVarCommandSource source = CVarCommandSource::Managed);
    // Atomically validates and enqueues one related managed command set.
    // Custom configuration intentionally uses the individual Set* APIs so a
    // conflict rejects only the affected item.
    [[nodiscard]] CVarBatchResult QueueBatch(
        std::span<const CVarWriteRequest> requests);
    // Reserves a name for an active specialized tweak that controls the value
    // without using the ordinary CVar writer (for example StreamingPoolFix).
    void ClaimManaged(std::wstring_view name);
    [[nodiscard]] bool ReadIntOnce(
        std::wstring_view name,
        IntReadCallback callback);

    [[nodiscard]] std::expected<void, std::string> Start(
        std::chrono::milliseconds period = std::chrono::milliseconds(100));
    void MarkUnavailable(std::string reason);
    void OnGameThreadTick();
    void Stop();

    [[nodiscard]] CVarSystemState State() const noexcept;
    [[nodiscard]] std::string UnavailableReason() const;

private:
    struct PendingWrite {
        std::wstring value;
        CVarValueKind kind = CVarValueKind::Opaque;
        CVarCommandSource source = CVarCommandSource::Managed;
        uint64_t generation = 0;
        CVarCommandTicket ticket;
    };

    struct CVarEntry {
        ScanEntry scanData;
        std::optional<ResolvedCVar> resolved;
        std::optional<PendingWrite> write;
        std::vector<IntReadCallback> intReads;
        std::optional<std::chrono::steady_clock::time_point> deadline;
    };

    using Cache = std::unordered_map<
        std::wstring,
        CVarEntry,
        CVarNameHash,
        CVarNameEqual>;

    struct SerializedWrite {
        std::wstring name;
        std::wstring value;
        CVarValueKind kind = CVarValueKind::Opaque;
    };

    struct ReadyReadBatch {
        std::wstring name;
        std::expected<int32_t, std::string> result;
        std::vector<IntReadCallback> callbacks;
    };

    CVarSystem();
    ~CVarSystem();

    CVarSystem(const CVarSystem&) = delete;
    CVarSystem& operator=(const CVarSystem&) = delete;
    CVarSystem(CVarSystem&&) = delete;
    CVarSystem& operator=(CVarSystem&&) = delete;

    [[nodiscard]] static CVarCommandTicket MakeTicket();
    static void CompleteTicket(
        const CVarCommandTicket& ticket,
        CVarCommandState state,
        std::string diagnostic = {},
        CVarSupersedeReason supersedeReason = CVarSupersedeReason::None);
    [[nodiscard]] static CVarQueueResult RejectedResult(
        std::string reason,
        CVarRejectionReason rejection);
    [[nodiscard]] static std::expected<SerializedWrite, std::string>
    SerializeRequest(const CVarWriteRequest& request);
    [[nodiscard]] CVarBatchResult QueueRequests(
        std::span<const CVarWriteRequest> requests,
        CVarCommandSource source);
    CVarEntry& EnsureEntryLocked(std::wstring_view name);
    [[nodiscard]] bool HasResolverWorkLocked() const;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    NearestDeadlineLocked() const;
    void ArmUnarmedDeadlinesLocked(std::chrono::steady_clock::time_point now);
    void FailAllCommandsLocked(
        std::string_view reason,
        std::vector<std::vector<IntReadCallback>>& callbackDiscard);
    void TerminalizeLocked(
        Cache::iterator entry,
        std::string reason,
        std::vector<Cache::node_type>& detachedEntries);
    void SweepExpiredLocked(
        std::chrono::steady_clock::time_point now,
        std::vector<Cache::node_type>& detachedEntries);

    [[nodiscard]] std::expected<void, std::string> InvokeStringSetter(
        const ResolvedCVar& resolved,
        std::wstring_view value) const;
    void ProcessPendingCommands();
    void ProcessPendingCommandForName(std::wstring_view name);
    void ProcessPendingReads();
    void ProcessStartupReconciliation(
        std::chrono::steady_clock::time_point now);
    [[nodiscard]] std::optional<int32_t> ReadResolvedValueWord(
        const ResolvedCVar& resolved) const;
    [[nodiscard]] const ModuleInfo* GetOrFetchModule();
    void PerformInitialScan();
    void ResolvePendingCVars(const ModuleInfo& module);
    void ResolverLoop(std::chrono::milliseconds period);
    void FinishGameThreadPass() noexcept;

#if defined(JST_UNIT_TESTS)
    void RecordLifecycleAttemptForTest();
    void RecordLifecycleEntryForTest();
#endif

    // Serializes lifecycle transactions and all resolver-thread ownership
    // operations. Code that needs both mutexes always acquires this first.
    std::mutex m_lifecycleMutex;
    mutable std::mutex m_mutex;
    std::condition_variable m_resolverCv;
    std::condition_variable m_stateCv;
    Cache m_cache;
    std::unordered_set<std::wstring, CVarNameHash, CVarNameEqual>
        m_managedClaims;
    std::vector<std::wstring> m_needsInitialScan;
    std::list<ReadyReadBatch> m_readyReadBatches;
    std::chrono::milliseconds m_pendingTimeout{30'000};
    uint64_t m_nextGeneration = 1;
    size_t m_activeGameThreadPasses = 0;
    CVarSystemState m_state = CVarSystemState::Stopped;
    bool m_gameThreadReady = false;
    std::string m_unavailableReason;

    std::optional<ModuleInfo> m_module;
    CVarStartupReconciler m_startupReconciler;

#if defined(JST_UNIT_TESTS)
    // Test-only sequencing/injection state for lifetime and allocation-failure
    // coverage. None of this is present in production builds.
    std::condition_variable m_testResolverCv;
    bool m_testPauseResolverAfterModuleFetch = false;
    bool m_testResolverPaused = false;
    bool m_testStopBeforeResolverJoin = false;
    std::optional<size_t> m_testResolvedReadDetachmentFailAfter;
    size_t m_testLifecycleAttempts = 0;
    size_t m_testLifecycleEntries = 0;

    friend class CVarSystemTestAccess;
#endif

    // Keep this last: reverse member destruction joins the resolver before the
    // state it may still inspect is destroyed.
    std::jthread m_resolverThread;
};

} // namespace jst::core
