#include "cvar_system.hpp"

#include "cvar_layout.hpp"
#include "cvar_overrides.hpp"
#include "cvar_watch_registry.hpp"
#include "logging.hpp"
#include "memory_scanner.hpp"
#include "pe_utils.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <exception>
#include <format>
#include <utility>

namespace jst::core {

namespace {

[[nodiscard]] std::string_view LastSetByName(uint32_t flags) noexcept {
    switch (flags & cvar_layout::kSetByMask) {
    case 0x00000000: return "Constructor";
    case 0x01000000: return "Scalability";
    case 0x02000000: return "GameSetting";
    case 0x03000000: return "ProjectSetting";
    case 0x04000000: return "DeviceProfile";
    case 0x05000000: return "SystemSettingsIni";
    case 0x06000000: return "ConsoleVariablesIni";
    case 0x07000000: return "Commandline";
    case 0x08000000: return "Code";
    case cvar_layout::kSetByConsole: return "Console";
    default: return "Unknown";
    }
}

[[nodiscard]] bool InvokeSetterNoexcept(
    uintptr_t setterAddress,
    uintptr_t object,
    const wchar_t* value) noexcept {
    __try {
        using Setter = void(__fastcall*)(uintptr_t, const wchar_t*, uint32_t);
        reinterpret_cast<Setter>(setterAddress)(
            object,
            value,
            cvar_layout::kSetByConsole);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] std::optional<std::wstring> FormatFloat(float value) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    char buffer[64]{};
    const auto converted = std::to_chars(
        std::begin(buffer),
        std::end(buffer),
        value,
        std::chars_format::general);
    if (converted.ec != std::errc{}) {
        return std::nullopt;
    }
    return utils::Utf8ToWide(std::string_view(
        buffer,
        static_cast<size_t>(converted.ptr - buffer)));
}

} // namespace

struct CVarCommandTicket::SharedState {
    mutable std::mutex mutex;
    CVarCommandState state = CVarCommandState::Pending;
    CVarSupersedeReason supersedeReason = CVarSupersedeReason::None;
    std::string diagnostic;
};

CVarCommandSnapshot CVarCommandTicket::Snapshot() const {
    if (!m_state) {
        return CVarCommandSnapshot{
            .state = CVarCommandState::Failed,
            .diagnostic = "empty CVar command ticket",
        };
    }
    std::lock_guard lock(m_state->mutex);
    return CVarCommandSnapshot{
        .state = m_state->state,
        .supersedeReason = m_state->supersedeReason,
        .diagnostic = m_state->diagnostic,
    };
}

CVarSystem::CVarSystem()
    : m_watches(std::make_unique<CVarWatchRegistry>()) {}

CVarSystem::~CVarSystem() {
    Stop();
}

CVarSystem& CVarSystem::Instance() {
    static CVarSystem instance;
    return instance;
}

CVarCommandTicket CVarSystem::MakeTicket() {
    return CVarCommandTicket(std::make_shared<CVarCommandTicket::SharedState>());
}

void CVarSystem::CompleteTicket(
    const CVarCommandTicket& ticket,
    CVarCommandState state,
    std::string diagnostic,
    CVarSupersedeReason supersedeReason) {
    if (!ticket.m_state) {
        return;
    }
    std::lock_guard lock(ticket.m_state->mutex);
    if (ticket.m_state->state != CVarCommandState::Pending) {
        return;
    }
    ticket.m_state->state = state;
    ticket.m_state->supersedeReason = supersedeReason;
    ticket.m_state->diagnostic = std::move(diagnostic);
}

CVarQueueResult CVarSystem::RejectedResult(
    std::string reason,
    CVarRejectionReason rejection) {
    auto ticket = MakeTicket();
    CompleteTicket(ticket, CVarCommandState::Failed, std::move(reason));
    return CVarQueueResult{
        .result = CVarSetResult::Rejected,
        .ticket = std::move(ticket),
        .rejection = rejection,
    };
}

CVarWatchSubscription::~CVarWatchSubscription() {
    Reset();
}

CVarWatchSubscription::CVarWatchSubscription(
    CVarWatchSubscription&& other) noexcept
    : m_control(std::move(other.m_control)) {}

CVarWatchSubscription& CVarWatchSubscription::operator=(
    CVarWatchSubscription&& other) noexcept {
    if (this != &other) {
        Reset();
        m_control = std::move(other.m_control);
    }
    return *this;
}

void CVarWatchSubscription::Reset() {
    if (m_control) {
        m_control->CancelAndWait();
    }
    m_control.reset();
}

CVarSystemState CVarSystem::State() const noexcept {
    std::lock_guard lock(m_mutex);
    return m_state;
}

std::string CVarSystem::UnavailableReason() const {
    std::lock_guard lock(m_mutex);
    return m_unavailableReason;
}

const ModuleInfo* CVarSystem::GetOrFetchModule() {
    if (!m_module) {
        m_module = GetGameModuleInfo();
    }
    return m_module ? &*m_module : nullptr;
}

CVarQueueResult CVarSystem::SetString(
    std::wstring_view name,
    std::wstring_view value,
    CVarCommandSource source) {
    if (name.empty() || value.empty() ||
        name.find(L'\0') != std::wstring_view::npos ||
        value.find(L'\0') != std::wstring_view::npos) {
        return RejectedResult(
            "CVar name and value must be non-empty text",
            CVarRejectionReason::InvalidRequest);
    }
    return QueueWrite(name, std::wstring(value), source);
}

CVarQueueResult CVarSystem::SetInt(
    std::wstring_view name,
    int32_t value,
    CVarCommandSource source) {
    return SetString(name, std::to_wstring(value), source);
}

CVarQueueResult CVarSystem::SetFloat(
    std::wstring_view name,
    float value,
    CVarCommandSource source) {
    const auto formatted = FormatFloat(value);
    if (!formatted) {
        return RejectedResult(
            "CVar float must be finite and formattable",
            CVarRejectionReason::InvalidRequest);
    }
    return SetString(name, *formatted, source);
}

CVarSystem::CVarEntry& CVarSystem::EnsureEntryLocked(
    std::wstring_view name) {
    auto [found, inserted] = m_cache.try_emplace(std::wstring(name));
    if (inserted) {
        found->second.firstSeen = std::chrono::steady_clock::now();
        found->second.scanData.name = found->first;
        m_needsInitialScan.push_back(found->first);
    }
    return found->second;
}

CVarQueueResult CVarSystem::QueueWrite(
    std::wstring_view name,
    std::wstring value,
    CVarCommandSource source) {
    const std::array requests{CVarWriteRequest{
        .name = std::wstring(name),
        .value = std::move(value),
    }};
    auto batch = QueueRequests(requests, source);
    if (!batch.Accepted()) {
        JST_LOG_WARNING(
            "Rejected CVar write for '{}': {}.",
            utils::WideToUtf8(name),
            batch.diagnostic);
        return RejectedResult(
            std::move(batch.diagnostic),
            batch.rejection);
    }
    return std::move(batch.commands.front());
}

CVarBatchResult CVarSystem::QueueBatch(
    std::span<const CVarWriteRequest> requests) {
    return QueueRequests(requests, CVarCommandSource::Managed);
}

CVarBatchResult CVarSystem::QueueRequests(
    std::span<const CVarWriteRequest> requests,
    CVarCommandSource source) {
    if (requests.empty()) {
        return CVarBatchResult{
            .rejection = CVarRejectionReason::InvalidRequest,
            .diagnostic = "CVar batch must contain at least one command",
        };
    }

    const CVarNameEqual equal;
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        if (request.name.empty() || request.value.empty() ||
            request.name.find(L'\0') != std::wstring::npos ||
            request.value.find(L'\0') != std::wstring::npos) {
            return CVarBatchResult{
                .rejection = CVarRejectionReason::InvalidRequest,
                .diagnostic = "CVar name and value must be non-empty text",
            };
        }
        for (size_t duplicate = 0; duplicate < i; ++duplicate) {
            if (equal(request.name, requests[duplicate].name)) {
                return CVarBatchResult{
                    .rejection = CVarRejectionReason::InvalidRequest,
                    .diagnostic = std::format(
                        "CVar batch contains duplicate name '{}'",
                        utils::WideToUtf8(request.name)),
                };
            }
        }
    }

    CVarBatchResult queued;
    queued.commands.reserve(requests.size());
    std::vector<std::wstring> managedReplacements;
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running) {
            return CVarBatchResult{
                .rejection = CVarRejectionReason::SystemUnavailable,
                .diagnostic = m_unavailableReason.empty()
                    ? "CVar system is not running"
                    : m_unavailableReason,
            };
        }

        for (const auto& request : requests) {
            if (source == CVarCommandSource::Custom &&
                m_managedClaims.contains(request.name)) {
                return CVarBatchResult{
                    .rejection = CVarRejectionReason::ManagedConflict,
                    .diagnostic = std::format(
                        "'{}' is controlled by a specialized tweak",
                        utils::WideToUtf8(request.name)),
                };
            }
        }

        for (const auto& request : requests) {
            auto& entry = EnsureEntryLocked(request.name);
            auto ticket = MakeTicket();
            const bool replaced = entry.write.has_value();
            if (entry.write) {
                const bool managedPriority =
                    source == CVarCommandSource::Managed &&
                    entry.write->source == CVarCommandSource::Custom;
                if (managedPriority) {
                    managedReplacements.push_back(request.name);
                }
                CompleteTicket(
                    entry.write->ticket,
                    CVarCommandState::Superseded,
                    "replaced by a newer CVar value",
                    managedPriority
                        ? CVarSupersedeReason::ManagedPriority
                        : CVarSupersedeReason::NewerValue);
            }
            if (source == CVarCommandSource::Managed) {
                m_managedClaims.insert(request.name);
            }
            entry.write = PendingWrite{
                .value = request.value,
                .source = source,
                .generation = m_nextGeneration++,
                .ticket = ticket,
            };
            if (m_nextGeneration == 0) {
                m_nextGeneration = 1;
            }
            queued.commands.push_back(CVarQueueResult{
                .result = replaced
                    ? CVarSetResult::Replaced
                    : CVarSetResult::Queued,
                .ticket = std::move(ticket),
            });
        }
    }

    for (const auto& name : managedReplacements) {
        JST_LOG_WARNING(
            "Custom CVar '{}' was superseded by its specialized tweak.",
            utils::WideToUtf8(name));
    }
    m_resolverCv.notify_all();
    return queued;
}

void CVarSystem::ClaimManaged(std::wstring_view name) {
    if (name.empty() || name.find(L'\0') != std::wstring_view::npos) {
        return;
    }

    bool supersededCustom = false;
    {
        std::lock_guard lock(m_mutex);
        m_managedClaims.emplace(name);

        const auto found = m_cache.find(name);
        if (found != m_cache.end() && found->second.write &&
            found->second.write->source == CVarCommandSource::Custom) {
            CompleteTicket(
                found->second.write->ticket,
                CVarCommandState::Superseded,
                "claimed by a specialized tweak",
                CVarSupersedeReason::ManagedPriority);
            found->second.write.reset();
            supersededCustom = true;
        }
    }

    if (supersededCustom) {
        JST_LOG_WARNING(
            "Pending custom CVar '{}' was superseded by its specialized tweak.",
            utils::WideToUtf8(name));
    }
}

bool CVarSystem::HasResolverWorkLocked() const {
    if (!m_needsInitialScan.empty()) {
        return true;
    }
    return std::ranges::any_of(m_cache, [](const auto& item) {
        return !item.second.resolved.has_value();
    });
}

void CVarSystem::FailAllCommandsLocked(std::string_view reason) {
    for (auto& [name, entry] : m_cache) {
        (void)name;
        if (entry.write) {
            CompleteTicket(
                entry.write->ticket,
                CVarCommandState::Failed,
                std::string(reason));
            entry.write.reset();
        }
    }
}

bool CVarSystem::OpenGameSettingsBarrier() {
    std::lock_guard lock(m_mutex);
    if (m_state != CVarSystemState::Running) {
        return false;
    }
    m_gameSettingsReady = true;
    return true;
}

std::expected<void, std::string> CVarSystem::InvokeStringSetter(
    const ResolvedCVar& resolved,
    std::wstring_view name,
    std::wstring_view value) const {
    if (!ValidateResolvedCVar(resolved)) {
        return std::unexpected("resolved object or string setter is no longer valid");
    }

    const uint32_t before = utils::SafeReadInt32(
        resolved.cvarObject + cvar_layout::kFlagsOffset);
    const std::wstring terminated(value);
    if (!InvokeSetterNoexcept(
            resolved.stringSetter,
            resolved.cvarObject,
            terminated.c_str())) {
        return std::unexpected("SEH caught while invoking the Unreal string setter");
    }

    const uint32_t after = utils::SafeReadInt32(
        resolved.cvarObject + cvar_layout::kFlagsOffset);
    if ((after & cvar_layout::kSetByMask) != cvar_layout::kSetByConsole) {
        return std::unexpected(std::format(
            "setter returned with LastSetBy {} instead of Console",
            LastSetByName(after)));
    }

    JST_LOG_INFO(
        "Set CVar '{}' to '{}' via IConsoleVariable::Set ({} -> Console).",
        utils::WideToUtf8(name),
        utils::WideToUtf8(value),
        LastSetByName(before));
    return {};
}

void CVarSystem::ProcessPendingCommands() {
    struct WriteSnapshot {
        std::wstring name;
        ResolvedCVar resolved;
        PendingWrite write;
    };
    std::vector<WriteSnapshot> writes;
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running ||
            !m_gameSettingsReady) {
            return;
        }
        for (const auto& [name, entry] : m_cache) {
            if (!entry.resolved) {
                continue;
            }
            if (entry.write) {
                writes.push_back(WriteSnapshot{
                    .name = name,
                    .resolved = *entry.resolved,
                    .write = *entry.write,
                });
            }
        }
    }

    for (const auto& snapshot : writes) {
        const auto applied = InvokeStringSetter(
            snapshot.resolved,
            snapshot.name,
            snapshot.write.value);
        CompleteTicket(
            snapshot.write.ticket,
            applied ? CVarCommandState::Applied : CVarCommandState::Failed,
            applied ? std::string{} : applied.error());
        if (!applied) {
            JST_LOG_ERROR(
                "CVar write for '{}' failed: {}. It will not be retried.",
                utils::WideToUtf8(snapshot.name),
                applied.error());
        }

        {
            std::lock_guard lock(m_mutex);
            const auto found = m_cache.find(snapshot.name);
            if (found != m_cache.end() && found->second.write &&
                found->second.write->generation == snapshot.write.generation) {
                found->second.write.reset();
            }
        }
    }
}

CVarWatchSubscription CVarSystem::WatchInt(IntWatchRequest request) {
    if (request.name.empty() || !request.onValue ||
        request.timeout < std::chrono::milliseconds::zero()) {
        return {};
    }

    const std::wstring name = request.name;
    std::shared_ptr<CVarWatchControl> control;
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running) {
            return {};
        }
        // Registration is serialized with the lifecycle state transition, so
        // Stop cannot clear the registry and then lose a late insertion.
        control = m_watches->Register(std::move(request));
        (void)EnsureEntryLocked(name);
    }
    m_resolverCv.notify_all();
    return CVarWatchSubscription(std::move(control));
}

std::optional<int32_t> CVarSystem::ReadResolvedInt(
    std::wstring_view name) const {
    std::optional<ResolvedCVar> resolved;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_cache.find(name);
        if (found == m_cache.end()) {
            return std::nullopt;
        }
        resolved = found->second.resolved;
    }
    if (!resolved || !ValidateResolvedCVar(*resolved)) {
        return std::nullopt;
    }
    const uintptr_t address = ResolveCVarReadAddress(*resolved);
    if (!address || !utils::IsValidPointer(address)) {
        return std::nullopt;
    }
    return utils::SafeReadInt32(address);
}

void CVarSystem::FinishGameThreadPass() noexcept {
    std::lock_guard lock(m_mutex);
    --m_activeGameThreadPasses;
    m_stateCv.notify_all();
}

void CVarSystem::OnGameThreadTick() {
    struct PassGuard {
        CVarSystem* owner;
        ~PassGuard() { owner->FinishGameThreadPass(); }
    };

    bool watchBarrierOpen = false;
    bool openedWatchBarrier = false;
    std::chrono::steady_clock::time_point barrierTime{};
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running) {
            return;
        }
        ++m_activeGameThreadPasses;
        if (!m_gameThreadReady) {
            m_gameThreadReady = true;
            barrierTime = std::chrono::steady_clock::now();
            for (auto& [name, entry] : m_cache) {
                (void)name;
                entry.firstSeen = barrierTime;
            }
        }
        if (m_gameSettingsReady && !m_watchBarrierOpened) {
            m_watchBarrierOpened = true;
            barrierTime = std::chrono::steady_clock::now();
            openedWatchBarrier = true;
        }
        watchBarrierOpen = m_watchBarrierOpened;
    }
    const PassGuard guard{this};

    if (openedWatchBarrier) {
        m_watches->OpenStartupBarrier(barrierTime);
    }
    ProcessPendingCommands();

    if (!watchBarrierOpen) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= m_nextWatchEvaluation) {
        m_nextWatchEvaluation = now + std::chrono::milliseconds(100);
        m_watches->Evaluate([this](std::wstring_view name) {
            return ReadResolvedInt(name);
        });
    }
}

void CVarSystem::PerformInitialScan() {
    std::vector<std::wstring> names;
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running ||
            m_needsInitialScan.empty()) {
            return;
        }
        names = std::move(m_needsInitialScan);
        m_needsInitialScan.clear();
    }

    const auto* module = GetOrFetchModule();
    if (!module || module->base == 0) {
        std::lock_guard lock(m_mutex);
        m_needsInitialScan.insert(
            m_needsInitialScan.end(),
            std::make_move_iterator(names.begin()),
            std::make_move_iterator(names.end()));
        return;
    }

    std::vector<std::wstring_view> views;
    views.reserve(names.size());
    for (const auto& name : names) {
        views.push_back(name);
    }
    auto scanned = ScanForNames(views, *module);

    std::lock_guard lock(m_mutex);
    for (auto& result : scanned) {
        const auto found = m_cache.find(result.name);
        if (found != m_cache.end() && !found->second.resolved) {
            found->second.scanData = std::move(result);
        }
    }
    for (const auto& name : names) {
        const auto found = m_cache.find(name);
        if (found == m_cache.end() || found->second.resolved ||
            found->second.scanData.strAddr != 0) {
            continue;
        }
        if (found->second.write) {
            CompleteTicket(
                found->second.write->ticket,
                CVarCommandState::Failed,
                "CVar name was not found in the game binary");
        }
        JST_LOG_WARNING(
            "CVar '{}' was not found in the game binary. Dropped.",
            utils::WideToUtf8(name));
        // The watch registry owns its own timeout and cancellation state. It
        // does not need an empty cache placeholder to time out cleanly.
        m_cache.erase(found);
    }
}

void CVarSystem::ResolvePendingCVars(const ModuleInfo& module) {
    struct Snapshot {
        std::wstring name;
        ScanEntry scanData;
        const CVarOverride* overrideEntry = nullptr;
        std::chrono::steady_clock::time_point firstSeen{};
    };

    std::vector<Snapshot> snapshots;
    const auto now = std::chrono::steady_clock::now();
    bool gameThreadReady = false;
    {
        std::lock_guard lock(m_mutex);
        gameThreadReady = m_gameThreadReady;
        for (const auto& [name, entry] : m_cache) {
            if (entry.resolved || entry.scanData.strAddr == 0) {
                continue;
            }
            snapshots.push_back(Snapshot{
                .name = name,
                .scanData = entry.scanData,
                .overrideEntry = FindCVarOverride(name),
                .firstSeen = entry.firstSeen,
            });
        }
    }

    std::vector<std::pair<std::wstring, ResolvedCVar>> resolvedEntries;
    for (auto& snapshot : snapshots) {
        auto resolved = ResolveFromScan(
            snapshot.scanData,
            module,
            snapshot.overrideEntry);
        if (resolved) {
            resolvedEntries.emplace_back(std::move(snapshot.name), *resolved);
        }
    }

    std::lock_guard lock(m_mutex);
    for (const auto& [name, resolved] : resolvedEntries) {
        const auto found = m_cache.find(name);
        if (found != m_cache.end() && !found->second.resolved) {
            found->second.resolved = resolved;
            JST_LOG_DEBUG(
                "Resolved IConsoleVariable object for '{}'.",
                utils::WideToUtf8(name));
        }
    }

    for (auto it = m_cache.begin(); it != m_cache.end();) {
        auto& entry = it->second;
        if (entry.resolved || !gameThreadReady ||
            now - entry.firstSeen <= m_pendingTimeout) {
            ++it;
            continue;
        }
        if (entry.write) {
            CompleteTicket(
                entry.write->ticket,
                CVarCommandState::Failed,
                "CVar object resolution timed out");
            entry.write.reset();
        }
        if (!m_watches->HasFor(it->first)) {
            JST_LOG_WARNING(
                "CVar object resolution for '{}' timed out.",
                utils::WideToUtf8(it->first));
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void CVarSystem::ResolverLoop(std::chrono::milliseconds period) {
    while (true) {
        {
            std::unique_lock lock(m_mutex);
            if (!HasResolverWorkLocked()) {
                m_resolverCv.wait(lock, [this] {
                    return m_state != CVarSystemState::Running ||
                        HasResolverWorkLocked();
                });
            } else {
                m_resolverCv.wait_for(lock, period, [this] {
                    return m_state != CVarSystemState::Running;
                });
            }
            if (m_state != CVarSystemState::Running) {
                return;
            }
        }

        PerformInitialScan();
        if (const auto* module = GetOrFetchModule();
            module && module->base != 0) {
            ResolvePendingCVars(*module);
        }
    }
}

std::expected<void, std::string> CVarSystem::Start(
    std::chrono::milliseconds period) {
    std::lock_guard lock(m_mutex);
    if (m_state == CVarSystemState::Running) {
        return {};
    }
    if (m_state != CVarSystemState::Stopped) {
        return std::unexpected(
            m_unavailableReason.empty()
                ? "CVar system cannot start during its current lifecycle state"
                : m_unavailableReason);
    }
    if (period <= std::chrono::milliseconds::zero()) {
        return std::unexpected("CVar resolver period must be positive");
    }
    m_state = CVarSystemState::Running;
    m_gameThreadReady = false;
    m_gameSettingsReady = false;
    m_watchBarrierOpened = false;
    m_unavailableReason.clear();
    m_nextWatchEvaluation = {};
    try {
        m_resolverThread = std::jthread([this, period] {
            ResolverLoop(period);
        });
    } catch (const std::exception& error) {
        m_state = CVarSystemState::Stopped;
        return std::unexpected(std::format(
            "failed to start the CVar resolver thread: {}",
            error.what()));
    } catch (...) {
        m_state = CVarSystemState::Stopped;
        return std::unexpected(
            "failed to start the CVar resolver thread: unknown exception");
    }
    return {};
}

void CVarSystem::MarkUnavailable(std::string reason) {
    {
        std::unique_lock lock(m_mutex);
        if (m_state == CVarSystemState::Unavailable) {
            return;
        }
        m_state = CVarSystemState::Unavailable;
        m_unavailableReason = std::move(reason);
        m_resolverCv.notify_all();
        m_stateCv.wait(lock, [this] {
            return m_activeGameThreadPasses == 0;
        });
        FailAllCommandsLocked(m_unavailableReason);
        m_needsInitialScan.clear();
    }
    if (m_resolverThread.joinable()) {
        m_resolverThread.join();
    }
    m_watches->Clear();
    JST_LOG_ERROR("CVar system unavailable: {}", UnavailableReason());
}

void CVarSystem::Stop() {
    {
        std::unique_lock lock(m_mutex);
        if (m_state == CVarSystemState::Stopped) {
            return;
        }
        m_state = CVarSystemState::Stopping;
        m_resolverCv.notify_all();
        m_stateCv.wait(lock, [this] {
            return m_activeGameThreadPasses == 0;
        });
        FailAllCommandsLocked("CVar system stopped");
    }
    if (m_resolverThread.joinable()) {
        m_resolverThread.join();
    }
    m_watches->Clear();
    {
        std::lock_guard lock(m_mutex);
        m_cache.clear();
        m_managedClaims.clear();
        m_needsInitialScan.clear();
        m_module.reset();
        m_gameThreadReady = false;
        m_gameSettingsReady = false;
        m_watchBarrierOpened = false;
        m_state = CVarSystemState::Stopped;
    }
}

} // namespace jst::core
