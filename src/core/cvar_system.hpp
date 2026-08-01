#pragma once

#include "cvar_name.hpp"
#include "cvar_resolver.hpp"
#include "cvar_scanner.hpp"
#include "cvar_watch.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jst::core {

class CVarSystem;
class CVarWatchRegistry;
class GameSettingsBarrier;

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
    std::wstring value;
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
 * Resolves CVar objects away from the game thread. Engine reads, watch
 * callbacks, and setter commands all wait for both the first post-Tick and
 * the game's post-startup settings pass.
 */
class CVarSystem final {
public:
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
    [[nodiscard]] CVarWatchSubscription WatchInt(IntWatchRequest request);

    [[nodiscard]] std::expected<void, std::string> Start(
        std::chrono::milliseconds period = std::chrono::milliseconds(100));
    void MarkUnavailable(std::string reason);
    void OnGameThreadTick();
    void Stop();

    [[nodiscard]] CVarSystemState State() const noexcept;
    [[nodiscard]] std::string UnavailableReason() const;

private:
    friend class GameSettingsBarrier;

    struct PendingWrite {
        std::wstring value;
        CVarCommandSource source = CVarCommandSource::Managed;
        uint64_t generation = 0;
        CVarCommandTicket ticket;
    };

    struct CVarEntry {
        ScanEntry scanData;
        std::optional<ResolvedCVar> resolved;
        std::optional<PendingWrite> write;
        std::chrono::steady_clock::time_point firstSeen{};
    };

    using Cache = std::unordered_map<
        std::wstring,
        CVarEntry,
        CVarNameHash,
        CVarNameEqual>;

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
    [[nodiscard]] bool OpenGameSettingsBarrier();

    [[nodiscard]] CVarQueueResult QueueWrite(
        std::wstring_view name,
        std::wstring value,
        CVarCommandSource source);
    [[nodiscard]] CVarBatchResult QueueRequests(
        std::span<const CVarWriteRequest> requests,
        CVarCommandSource source);
    CVarEntry& EnsureEntryLocked(std::wstring_view name);
    [[nodiscard]] bool HasResolverWorkLocked() const;
    void FailAllCommandsLocked(std::string_view reason);

    [[nodiscard]] std::expected<void, std::string> InvokeStringSetter(
        const ResolvedCVar& resolved,
        std::wstring_view name,
        std::wstring_view value) const;
    void ProcessPendingCommands();
    [[nodiscard]] std::optional<int32_t> ReadResolvedInt(
        std::wstring_view name) const;
    [[nodiscard]] const ModuleInfo* GetOrFetchModule();
    void PerformInitialScan();
    void ResolvePendingCVars(const ModuleInfo& module);
    void ResolverLoop(std::chrono::milliseconds period);
    void FinishGameThreadPass() noexcept;

    mutable std::mutex m_mutex;
    std::condition_variable m_resolverCv;
    std::condition_variable m_stateCv;
    Cache m_cache;
    std::unordered_set<std::wstring, CVarNameHash, CVarNameEqual>
        m_managedClaims;
    std::vector<std::wstring> m_needsInitialScan;
    std::chrono::milliseconds m_pendingTimeout{30'000};
    uint64_t m_nextGeneration = 1;
    size_t m_activeGameThreadPasses = 0;
    CVarSystemState m_state = CVarSystemState::Stopped;
    bool m_gameThreadReady = false;
    bool m_gameSettingsReady = false;
    bool m_watchBarrierOpened = false;
    std::string m_unavailableReason;

    std::optional<ModuleInfo> m_module;
    std::unique_ptr<CVarWatchRegistry> m_watches;
    std::jthread m_resolverThread;
    std::chrono::steady_clock::time_point m_nextWatchEvaluation{};

#if defined(JST_UNIT_TESTS)
    friend class CVarSystemTestAccess;
#endif
};

} // namespace jst::core
