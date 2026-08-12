#include "cvar_system.hpp"

#include "cvar_layout.hpp"
#include "cvar_overrides.hpp"
#include "logging.hpp"
#include "memory_scanner.hpp"
#include "pe_utils.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <exception>
#include <format>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace jst::core {

namespace {

constexpr auto kStartupReconcileDuration = std::chrono::seconds(15);
constexpr auto kStartupReconcileInterval = std::chrono::milliseconds(100);
constexpr std::string_view kInvalidResolvedCVar =
    "resolved object or string setter is no longer valid";

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
        std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (converted.ec != std::errc{}) {
        return std::nullopt;
    }
    return utils::Utf8ToWide(std::string_view(
        buffer, static_cast<size_t>(converted.ptr - buffer)));
}

[[nodiscard]] std::string FormatObservedValue(
    CVarValueKind kind,
    int32_t valueWord) {
    switch (kind) {
    case CVarValueKind::Integer:
        return std::to_string(valueWord);
    case CVarValueKind::Float:
        if (const auto formatted = FormatFloat(std::bit_cast<float>(valueWord))) {
            return utils::WideToUtf8(*formatted);
        }
        break;
    case CVarValueKind::Opaque:
        break;
    }
    return std::format("0x{:08X}", static_cast<uint32_t>(valueWord));
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
        return {.state = CVarCommandState::Failed,
                .diagnostic = "empty CVar command ticket"};
    }
    std::lock_guard lock(m_state->mutex);
    return {.state = m_state->state,
            .supersedeReason = m_state->supersedeReason,
            .diagnostic = m_state->diagnostic};
}

CVarSystem::CVarSystem() = default;

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
    return {.result = CVarSetResult::Rejected,
            .ticket = std::move(ticket),
            .rejection = rejection};
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
    const std::array requests{CVarWriteRequest{
        .name = std::wstring(name), .value = std::wstring(value)}};
    auto batch = QueueRequests(requests, source);
    if (!batch.Accepted()) {
        return RejectedResult(std::move(batch.diagnostic), batch.rejection);
    }
    return std::move(batch.commands.front());
}

CVarQueueResult CVarSystem::SetInt(
    std::wstring_view name,
    int32_t value,
    CVarCommandSource source) {
    const std::array requests{CVarWriteRequest{
        .name = std::wstring(name), .value = value}};
    auto batch = QueueRequests(requests, source);
    if (!batch.Accepted()) {
        return RejectedResult(std::move(batch.diagnostic), batch.rejection);
    }
    return std::move(batch.commands.front());
}

CVarQueueResult CVarSystem::SetFloat(
    std::wstring_view name,
    float value,
    CVarCommandSource source) {
    const std::array requests{CVarWriteRequest{
        .name = std::wstring(name), .value = value}};
    auto batch = QueueRequests(requests, source);
    if (!batch.Accepted()) {
        return RejectedResult(std::move(batch.diagnostic), batch.rejection);
    }
    return std::move(batch.commands.front());
}

std::expected<CVarSystem::SerializedWrite, std::string>
CVarSystem::SerializeRequest(const CVarWriteRequest& request) {
    if (request.name.empty() ||
        request.name.find(L'\0') != std::wstring::npos) {
        return std::unexpected("CVar name must be non-empty text");
    }
    return std::visit([&](const auto& value)
        -> std::expected<SerializedWrite, std::string> {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::wstring>) {
            if (value.empty() || value.find(L'\0') != std::wstring::npos) {
                return std::unexpected("CVar string value must be non-empty text");
            }
            return SerializedWrite{
                .name = request.name,
                .value = value,
                .kind = CVarValueKind::Opaque};
        } else if constexpr (std::is_same_v<Value, int32_t>) {
            return SerializedWrite{
                .name = request.name,
                .value = std::to_wstring(value),
                .kind = CVarValueKind::Integer};
        } else {
            const auto formatted = FormatFloat(value);
            if (!formatted) {
                return std::unexpected("CVar float must be finite and formattable");
            }
            return SerializedWrite{
                .name = request.name,
                .value = *formatted,
                .kind = CVarValueKind::Float};
        }
    }, request.value);
}

CVarBatchResult CVarSystem::QueueBatch(
    std::span<const CVarWriteRequest> requests) {
    return QueueRequests(requests, CVarCommandSource::Managed);
}

CVarBatchResult CVarSystem::QueueRequests(
    std::span<const CVarWriteRequest> requests,
    CVarCommandSource source) {
    if (requests.empty()) {
        return {.rejection = CVarRejectionReason::InvalidRequest,
                .diagnostic = "CVar batch must contain at least one command"};
    }

    std::vector<SerializedWrite> serialized;
    serialized.reserve(requests.size());
    const CVarNameEqual equal;
    for (const auto& request : requests) {
        auto converted = SerializeRequest(request);
        if (!converted) {
            return {.rejection = CVarRejectionReason::InvalidRequest,
                    .diagnostic = converted.error()};
        }
        for (const auto& existing : serialized) {
            if (equal(existing.name, converted->name)) {
                return {.rejection = CVarRejectionReason::InvalidRequest,
                        .diagnostic = std::format(
                            "CVar batch contains duplicate name '{}'",
                            utils::WideToUtf8(converted->name))};
            }
        }
        serialized.push_back(std::move(*converted));
    }

    CVarBatchResult queued;
    queued.commands.reserve(serialized.size());
    std::vector<std::wstring> managedReplacements;
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running) {
            return {.rejection = CVarRejectionReason::SystemUnavailable,
                    .diagnostic = m_unavailableReason.empty()
                        ? "CVar system is not running"
                        : m_unavailableReason};
        }
        if (m_nextGeneration == 0 ||
            serialized.size() >
                std::numeric_limits<uint64_t>::max() - m_nextGeneration + 1) {
            return {.rejection = CVarRejectionReason::InvalidRequest,
                    .diagnostic = "CVar command generation space is exhausted"};
        }
        for (const auto& request : serialized) {
            if (source == CVarCommandSource::Custom &&
                m_managedClaims.contains(request.name)) {
                return {.rejection = CVarRejectionReason::ManagedConflict,
                        .diagnostic = std::format(
                            "'{}' is controlled by a specialized tweak",
                            utils::WideToUtf8(request.name))};
            }
        }

        for (const auto& request : serialized) {
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
                    managedPriority ? CVarSupersedeReason::ManagedPriority
                                    : CVarSupersedeReason::NewerValue);
            }
            if (source == CVarCommandSource::Managed) {
                m_managedClaims.insert(request.name);
            }
            const uint64_t generation = m_nextGeneration;
            m_nextGeneration = generation == std::numeric_limits<uint64_t>::max()
                ? 0
                : generation + 1;
            entry.write = PendingWrite{
                .value = request.value,
                .kind = request.kind,
                .source = source,
                .generation = generation,
                .ticket = ticket};
            // Before the barrier this records the frozen startup value.  Once
            // it is open, Track performs the required live-command cancellation.
            m_startupReconciler.Track(
                request.name, request.value, request.kind, generation);
            queued.commands.push_back({
                .result = replaced ? CVarSetResult::Replaced : CVarSetResult::Queued,
                .ticket = std::move(ticket)});
        }
    }
    for (const auto& name : managedReplacements) {
        JST_LOG_WARNING("Custom CVar '{}' was superseded by its specialized tweak.",
                        utils::WideToUtf8(name));
    }
    m_resolverCv.notify_all();
    return queued;
}

CVarSystem::CVarEntry& CVarSystem::EnsureEntryLocked(
    std::wstring_view name) {
    auto [found, inserted] = m_cache.try_emplace(std::wstring(name));
    if (inserted) {
        found->second.scanData.name = found->first;
        if (m_gameThreadReady) {
            found->second.deadline =
                std::chrono::steady_clock::now() + m_pendingTimeout;
        }
        m_needsInitialScan.push_back(found->first);
    }
    return found->second;
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
            CompleteTicket(found->second.write->ticket,
                           CVarCommandState::Superseded,
                           "claimed by a specialized tweak",
                           CVarSupersedeReason::ManagedPriority);
            found->second.write.reset();
            supersededCustom = true;
        }
        // Specialized controllers own their own lifecycle and are excluded
        // from the generic startup reconciler even if they only issue reads.
        m_startupReconciler.Cancel(name);
    }
    if (supersededCustom) {
        JST_LOG_WARNING("Pending custom CVar '{}' was superseded by its specialized tweak.",
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

std::optional<std::chrono::steady_clock::time_point>
CVarSystem::NearestDeadlineLocked() const {
    std::optional<std::chrono::steady_clock::time_point> nearest;
    for (const auto& [name, entry] : m_cache) {
        (void)name;
        if (entry.resolved || !entry.deadline ||
            (!entry.write && entry.intReads.empty())) {
            continue;
        }
        if (!nearest || *entry.deadline < *nearest) {
            nearest = entry.deadline;
        }
    }
    return nearest;
}

void CVarSystem::ArmUnarmedDeadlinesLocked(
    std::chrono::steady_clock::time_point now) {
    for (auto& [name, entry] : m_cache) {
        (void)name;
        if (!entry.resolved && !entry.deadline) {
            entry.deadline = now + m_pendingTimeout;
        }
    }
}

void CVarSystem::TerminalizeLocked(
    Cache::iterator entry,
    std::string reason,
    std::vector<Cache::node_type>& detachedEntries) {
    if (entry->second.write) {
        CompleteTicket(entry->second.write->ticket,
                       CVarCommandState::Failed,
                       reason);
        entry->second.write.reset();
    }
    m_startupReconciler.Cancel(entry->first);
    if (!entry->second.intReads.empty()) {
        m_readyReadBatches.emplace_back();
        auto& batch = m_readyReadBatches.back();
        batch.name = entry->first;
        batch.result = std::unexpected(reason);
        batch.callbacks.swap(entry->second.intReads);
    }
    std::erase_if(m_needsInitialScan, [&](const std::wstring& candidate) {
        return CVarNameEqual{}(candidate, entry->first);
    });
    detachedEntries.push_back(m_cache.extract(entry));
}

void CVarSystem::SweepExpiredLocked(
    std::chrono::steady_clock::time_point now,
    std::vector<Cache::node_type>& detachedEntries) {
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        if (it->second.resolved || !it->second.deadline ||
            now < *it->second.deadline) {
            ++it;
            continue;
        }
        auto current = it++;
        TerminalizeLocked(current, "CVar object resolution timed out", detachedEntries);
    }
}

void CVarSystem::FailAllCommandsLocked(
    std::string_view reason,
    std::vector<std::vector<IntReadCallback>>& callbackDiscard) {
    for (auto& [name, entry] : m_cache) {
        (void)name;
        if (entry.write) {
            CompleteTicket(entry.write->ticket, CVarCommandState::Failed,
                           std::string(reason));
            entry.write.reset();
        }
        if (!entry.intReads.empty()) {
            callbackDiscard.emplace_back();
            callbackDiscard.back().swap(entry.intReads);
        }
    }
    for (auto& batch : m_readyReadBatches) {
        if (!batch.callbacks.empty()) {
            callbackDiscard.emplace_back();
            callbackDiscard.back().swap(batch.callbacks);
        }
    }
    m_readyReadBatches.clear();
    m_startupReconciler.Clear();
}

std::expected<void, std::string> CVarSystem::InvokeStringSetter(
    const ResolvedCVar& resolved,
    std::wstring_view value) const {
    if (!ValidateResolvedCVar(resolved)) {
        return std::unexpected(std::string(kInvalidResolvedCVar));
    }
    const std::wstring terminated(value);
    if (!InvokeSetterNoexcept(resolved.stringSetter, resolved.cvarObject,
                              terminated.c_str())) {
        return std::unexpected("SEH caught while invoking the Unreal string setter");
    }
    const uint32_t after = utils::SafeReadInt32(
        resolved.cvarObject + cvar_layout::kFlagsOffset);
    if ((after & cvar_layout::kSetByMask) != cvar_layout::kSetByConsole) {
        return std::unexpected(std::format(
            "setter returned with LastSetBy {} instead of Console", LastSetByName(after)));
    }
    return {};
}

void CVarSystem::ProcessPendingCommands() {
    std::vector<std::wstring> names;
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running || !m_gameThreadReady) {
            return;
        }
        for (const auto& [name, entry] : m_cache) {
            if (entry.resolved && entry.write) {
                names.push_back(name);
            }
        }
    }
    for (const auto& name : names) {
        ProcessPendingCommandForName(name);
    }
}

void CVarSystem::ProcessPendingCommandForName(std::wstring_view name) {
    struct Snapshot {
        std::wstring name;
        ResolvedCVar resolved;
        PendingWrite write;
    };
    std::optional<Snapshot> snapshot;
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running || !m_gameThreadReady) {
            return;
        }
        const auto found = m_cache.find(name);
        if (found == m_cache.end() || !found->second.resolved ||
            !found->second.write) {
            return;
        }
        snapshot = Snapshot{.name = found->first,
                            .resolved = *found->second.resolved,
                            .write = *found->second.write};
    }

    const bool validBeforeCall = ValidateResolvedCVar(snapshot->resolved);
    const auto previousValue = validBeforeCall
        ? ReadResolvedValueWord(snapshot->resolved) : std::nullopt;
    const auto applied = InvokeStringSetter(snapshot->resolved, snapshot->write.value);
    const auto appliedValue = applied
        ? ReadResolvedValueWord(snapshot->resolved) : std::nullopt;
    CompleteTicket(snapshot->write.ticket,
                   applied ? CVarCommandState::Applied : CVarCommandState::Failed,
                   applied ? std::string{} : applied.error());

    {
        std::lock_guard lock(m_mutex);
        const auto found = m_cache.find(snapshot->name);
        if (found != m_cache.end() && found->second.write &&
            found->second.write->generation == snapshot->write.generation) {
            if (applied) {
                m_startupReconciler.CaptureApplied(
                    snapshot->name, snapshot->write.generation, appliedValue);
            } else {
                (void)m_startupReconciler.Cancel(
                    snapshot->name, snapshot->write.generation);
            }
            found->second.write.reset();
        }
    }
    if (applied) {
        JST_LOG_INFO("CVar write | name='{}' | previous={} | target={} | result=applied.",
                     utils::WideToUtf8(snapshot->name),
                     previousValue ? FormatObservedValue(snapshot->write.kind, *previousValue)
                                   : "unavailable",
                     utils::WideToUtf8(snapshot->write.value));
    } else {
        JST_LOG_ERROR("CVar write | name='{}' | target={} | result=failed | reason='{}'.",
                      utils::WideToUtf8(snapshot->name),
                      utils::WideToUtf8(snapshot->write.value), applied.error());
    }
}

std::optional<int32_t> CVarSystem::ReadResolvedValueWord(
    const ResolvedCVar& resolved) const {
    if (!ValidateResolvedCVar(resolved)) {
        return std::nullopt;
    }
    const uintptr_t address = ResolveCVarReadAddress(resolved);
    if (!address || !utils::IsValidPointer(address)) {
        return std::nullopt;
    }
    return utils::SafeReadInt32(address);
}

void CVarSystem::ProcessStartupReconciliation(
    std::chrono::steady_clock::time_point now) {
    CVarStartupEvaluation evaluation;
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running) {
            return;
        }
        evaluation = m_startupReconciler.Evaluate(now);
    }
    if (evaluation.finishedCorrectionCount) {
        JST_LOG_INFO("CVar startup sync | finished | corrections={}.",
                     *evaluation.finishedCorrectionCount);
        return;
    }

    for (const auto& target : evaluation.targets) {
        std::optional<ResolvedCVar> resolved;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_cache.find(target.name);
            if (found == m_cache.end() || found->second.write ||
                !found->second.resolved ||
                !m_startupReconciler.IsCurrent(target.name, target.generation)) {
                continue;
            }
            resolved = *found->second.resolved;
        }
        const auto currentValue = ReadResolvedValueWord(*resolved);
        if (!currentValue || !ValidateResolvedCVar(*resolved)) {
            std::lock_guard lock(m_mutex);
            (void)m_startupReconciler.Cancel(target.name, target.generation);
            JST_LOG_ERROR("CVar startup sync | name='{}' | result=failed | reason='{}'.",
                          utils::WideToUtf8(target.name), kInvalidResolvedCVar);
            continue;
        }
        if (*currentValue == target.expectedValueWord) {
            continue;
        }

        // This permit CAS is the last state transition before the unlocked
        // engine setter. A live admission races through Cancel while holding
        // the same cache mutex and wins when it changes the permit first.
        bool invoking = false;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_cache.find(target.name);
            if (found != m_cache.end() && !found->second.write &&
                found->second.resolved &&
                m_startupReconciler.IsCurrent(target.name, target.generation)) {
                invoking = m_startupReconciler.TryBeginCorrection(
                    target.name, target.generation, target.permit);
            }
        }
        if (!invoking || target.permit->load(std::memory_order_acquire) !=
                             CVarCorrectionPermitState::Invoking) {
            continue;
        }

        const auto applied = InvokeStringSetter(*resolved, target.value);
        const auto correctedValue = applied
            ? ReadResolvedValueWord(*resolved) : std::nullopt;
        bool recorded = false;
        {
            std::lock_guard lock(m_mutex);
            recorded = m_startupReconciler.RecordCorrection(
                target.name, target.generation, target.permit, correctedValue);
        }
        if (applied && recorded) {
            JST_LOG_INFO("CVar startup sync | name='{}' | previous={} | target={} | result=reapplied.",
                         utils::WideToUtf8(target.name),
                         FormatObservedValue(target.kind, *currentValue),
                         utils::WideToUtf8(target.value));
        } else if (!applied) {
            JST_LOG_ERROR("CVar startup sync | name='{}' | result=failed | reason='{}'.",
                          utils::WideToUtf8(target.name), applied.error());
        }
        // A command admitted while the setter ran supersedes this correction.
        // Apply one current value in this same post-Tick pass; a reentrant
        // replacement from that tail remains pending for the next pass.
        if (!recorded) {
            ProcessPendingCommandForName(target.name);
        }
    }
}

bool CVarSystem::ReadIntOnce(std::wstring_view name, IntReadCallback callback) {
    if (name.empty() || name.find(L'\0') != std::wstring_view::npos || !callback) {
        return false;
    }
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running) {
            return false;
        }
        auto& entry = EnsureEntryLocked(name);
        entry.intReads.push_back(std::move(callback));
    }
    m_resolverCv.notify_all();
    return true;
}

void CVarSystem::ProcessPendingReads() {
    struct ResolvedReadBatch {
        std::wstring name;
        ResolvedCVar resolved;
        std::expected<int32_t, std::string> result;
        std::vector<IntReadCallback> callbacks;

        ResolvedReadBatch(
            const std::wstring& sourceName,
            const ResolvedCVar& sourceResolved)
            : name(sourceName),
              resolved(sourceResolved),
              result(std::unexpected(std::string("CVar value is not readable"))) {}
    };
    std::list<ReadyReadBatch> ready;
    // Each list node fully owns its name and fallback result before callbacks
    // are detached. If preparation later fails, the catch block restores every
    // prior cohort without destroying any callback capture while m_mutex is
    // held.
    std::list<ResolvedReadBatch> reads;
    bool allocationDeferred = false;
    const auto invokeCallbacks = [](
                                     std::wstring_view name,
                                     const std::expected<int32_t, std::string>& result,
                                     std::vector<IntReadCallback>& callbacks) {
        for (auto& callback : callbacks) {
            try {
                callback(result);
            } catch (const std::exception& error) {
                JST_LOG_ERROR("CVar read callback for '{}' threw: {}.",
                              utils::WideToUtf8(name), error.what());
            } catch (...) {
                JST_LOG_ERROR("CVar read callback for '{}' threw an unknown exception.",
                              utils::WideToUtf8(name));
            }
        }
    };
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running || !m_gameThreadReady) {
            return;
        }
        ready.swap(m_readyReadBatches);
        const auto restoreDetachedReads = [&] noexcept {
            for (auto& read : reads) {
                const auto entry = m_cache.find(read.name);
                if (entry != m_cache.end()) {
                    entry->second.intReads.swap(read.callbacks);
                }
            }
            m_readyReadBatches.swap(ready);
        };
        try {
            for (auto& [name, entry] : m_cache) {
                if (!entry.resolved || entry.intReads.empty()) {
                    continue;
                }
#if defined(JST_UNIT_TESTS)
                if (m_testResolvedReadDetachmentFailAfter) {
                    if (*m_testResolvedReadDetachmentFailAfter == 0) {
                        m_testResolvedReadDetachmentFailAfter.reset();
                        throw std::bad_alloc();
                    }
                    --*m_testResolvedReadDetachmentFailAfter;
                }
#endif
                reads.emplace_back(name, *entry.resolved);
                auto& batch = reads.back();
                batch.callbacks.swap(entry.intReads);
            }
        } catch (const std::bad_alloc&) {
            restoreDetachedReads();
            allocationDeferred = true;
        } catch (...) {
            restoreDetachedReads();
            throw;
        }
    }
    if (allocationDeferred) {
        return;
    }
    for (auto& batch : ready) {
        invokeCallbacks(batch.name, batch.result, batch.callbacks);
    }
    for (auto& read : reads) {
        if (const auto value = ReadResolvedValueWord(read.resolved)) {
            read.result = *value;
        }
        invokeCallbacks(read.name, read.result, read.callbacks);
    }
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
    bool firstGameThreadPass = false;
    size_t startupTargetCount = 0;
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running) {
            return;
        }
        ++m_activeGameThreadPasses;
        if (!m_gameThreadReady) {
            m_gameThreadReady = true;
            firstGameThreadPass = true;
            const auto barrierTime = std::chrono::steady_clock::now();
            ArmUnarmedDeadlinesLocked(barrierTime);
            startupTargetCount = m_startupReconciler.Start(
                barrierTime,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    kStartupReconcileDuration),
                kStartupReconcileInterval);
        }
    }
    const PassGuard guard{this};
    if (firstGameThreadPass) {
        m_resolverCv.notify_all();
        JST_LOG_INFO("CVar startup sync | started | targets={} | duration=15s | interval=100ms.",
                     startupTargetCount);
    }
    ProcessPendingCommands();
    ProcessPendingReads();
    ProcessStartupReconciliation(std::chrono::steady_clock::now());
}

void CVarSystem::PerformInitialScan() {
    std::vector<std::wstring> names;
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running) {
            return;
        }
        names = std::move(m_needsInitialScan);
        m_needsInitialScan.clear();
    }

    std::vector<Cache::node_type> detachedEntries;
    const auto now = std::chrono::steady_clock::now();
    const auto* module = GetOrFetchModule();
    if (!module || module->base == 0) {
        std::lock_guard lock(m_mutex);
        for (const auto& name : names) {
            if (m_cache.contains(name)) {
                m_needsInitialScan.push_back(name);
            }
        }
        SweepExpiredLocked(now, detachedEntries);
        return;
    }
    if (names.empty()) {
        std::lock_guard lock(m_mutex);
        SweepExpiredLocked(now, detachedEntries);
        return;
    }

    std::vector<std::wstring_view> views;
    views.reserve(names.size());
    for (const auto& name : names) {
        views.push_back(name);
    }
    auto scanned = ScanForNames(views, *module);
    std::unordered_set<std::wstring, CVarNameHash, CVarNameEqual> foundNames;
    for (const auto& entry : scanned) {
        foundNames.insert(entry.name);
    }
    {
        std::lock_guard lock(m_mutex);
        for (auto& result : scanned) {
            const auto found = m_cache.find(result.name);
            if (found != m_cache.end() && !found->second.resolved) {
                found->second.scanData = std::move(result);
            }
        }
        for (const auto& name : names) {
            const auto entry = m_cache.find(name);
            if (entry != m_cache.end() && !entry->second.resolved &&
                !foundNames.contains(name)) {
                TerminalizeLocked(entry,
                                  "CVar name was not found in the game binary",
                                  detachedEntries);
            }
        }
        SweepExpiredLocked(now, detachedEntries);
    }
}

void CVarSystem::ResolvePendingCVars(const ModuleInfo& module) {
    struct Snapshot {
        std::wstring name;
        ScanEntry scanData;
        const CVarOverride* overrideEntry = nullptr;
    };
    std::vector<Snapshot> snapshots;
    {
        std::lock_guard lock(m_mutex);
        if (m_state != CVarSystemState::Running) {
            return;
        }
        for (const auto& [name, entry] : m_cache) {
            if (!entry.resolved && entry.scanData.strAddr != 0) {
                snapshots.push_back({.name = name,
                                     .scanData = entry.scanData,
                                     .overrideEntry = FindCVarOverride(name)});
            }
        }
    }
    std::vector<std::pair<std::wstring, ResolvedCVar>> resolvedEntries;
    for (const auto& snapshot : snapshots) {
        if (const auto resolved = ResolveFromScan(
                snapshot.scanData, module, snapshot.overrideEntry)) {
            resolvedEntries.emplace_back(snapshot.name, *resolved);
        }
    }
    std::vector<Cache::node_type> detachedEntries;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& [name, resolved] : resolvedEntries) {
            const auto found = m_cache.find(name);
            if (found != m_cache.end() && !found->second.resolved) {
                found->second.resolved = resolved;
                found->second.deadline.reset();
                JST_LOG_DEBUG("Resolved IConsoleVariable object for '{}'.",
                              utils::WideToUtf8(name));
            }
        }
        SweepExpiredLocked(std::chrono::steady_clock::now(), detachedEntries);
    }
}

void CVarSystem::ResolverLoop(std::chrono::milliseconds period) {
    while (true) {
        {
            std::unique_lock lock(m_mutex);
            if (!HasResolverWorkLocked()) {
                m_resolverCv.wait(lock, [this] {
                    return m_state != CVarSystemState::Running || HasResolverWorkLocked();
                });
            } else {
                auto wait = period;
                if (const auto deadline = NearestDeadlineLocked()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (*deadline <= now) {
                        wait = std::chrono::milliseconds::zero();
                    } else {
                        wait = std::min(wait, std::chrono::duration_cast<std::chrono::milliseconds>(
                            *deadline - now));
                    }
                }
                m_resolverCv.wait_for(lock, wait, [this] {
                    return m_state != CVarSystemState::Running;
                });
            }
            if (m_state != CVarSystemState::Running) {
                return;
            }
        }
        PerformInitialScan();
        const auto* module = GetOrFetchModule();
#if defined(JST_UNIT_TESTS)
        {
            std::unique_lock lock(m_mutex);
            if (m_testPauseResolverAfterModuleFetch) {
                m_testResolverPaused = true;
                m_testResolverCv.notify_all();
                m_testResolverCv.wait(lock, [this] {
                    return !m_testPauseResolverAfterModuleFetch;
                });
            }
        }
#endif
        if (module && module->base != 0) {
            ResolvePendingCVars(*module);
        } else {
            std::vector<Cache::node_type> detachedEntries;
            std::lock_guard lock(m_mutex);
            SweepExpiredLocked(std::chrono::steady_clock::now(), detachedEntries);
        }
    }
}

#if defined(JST_UNIT_TESTS)
void CVarSystem::RecordLifecycleAttemptForTest() {
    {
        std::lock_guard lock(m_mutex);
        ++m_testLifecycleAttempts;
    }
    m_testResolverCv.notify_all();
}

void CVarSystem::RecordLifecycleEntryForTest() {
    {
        std::lock_guard lock(m_mutex);
        ++m_testLifecycleEntries;
    }
    m_testResolverCv.notify_all();
}
#endif

std::expected<void, std::string> CVarSystem::Start(
    std::chrono::milliseconds period) {
#if defined(JST_UNIT_TESTS)
    RecordLifecycleAttemptForTest();
#endif
    std::lock_guard lifecycleLock(m_lifecycleMutex);
#if defined(JST_UNIT_TESTS)
    RecordLifecycleEntryForTest();
#endif
    std::lock_guard lock(m_mutex);
    if (m_state == CVarSystemState::Running) {
        return {};
    }
    if (m_state != CVarSystemState::Stopped) {
        return std::unexpected(m_unavailableReason.empty()
            ? "CVar system cannot start during its current lifecycle state"
            : m_unavailableReason);
    }
    if (period <= std::chrono::milliseconds::zero()) {
        return std::unexpected("CVar resolver period must be positive");
    }
    m_state = CVarSystemState::Running;
    m_gameThreadReady = false;
    m_startupReconciler.Clear();
    m_unavailableReason.clear();
    try {
        m_resolverThread = std::jthread([this, period] { ResolverLoop(period); });
    } catch (const std::exception& error) {
        m_state = CVarSystemState::Stopped;
        return std::unexpected(std::format(
            "failed to start the CVar resolver thread: {}", error.what()));
    } catch (...) {
        m_state = CVarSystemState::Stopped;
        return std::unexpected("failed to start the CVar resolver thread: unknown exception");
    }
    return {};
}

void CVarSystem::MarkUnavailable(std::string reason) {
    std::vector<std::vector<IntReadCallback>> callbackDiscard;
    Cache discardedEntries;
    std::string acceptedReason;
#if defined(JST_UNIT_TESTS)
    RecordLifecycleAttemptForTest();
#endif
    {
        std::unique_lock lifecycleLock(m_lifecycleMutex);
#if defined(JST_UNIT_TESTS)
        RecordLifecycleEntryForTest();
#endif
        {
            std::unique_lock lock(m_mutex);
            if (m_state == CVarSystemState::Unavailable) {
                return;
            }
            const bool wasRunning = m_state == CVarSystemState::Running;
            m_state = CVarSystemState::Unavailable;
            m_unavailableReason = std::move(reason);
            acceptedReason = m_unavailableReason;
            if (wasRunning) {
                m_resolverCv.notify_all();
                m_stateCv.wait(lock, [this] { return m_activeGameThreadPasses == 0; });
                FailAllCommandsLocked(m_unavailableReason, callbackDiscard);
                discardedEntries.swap(m_cache);
                m_managedClaims.clear();
                m_needsInitialScan.clear();
            }
        }
        if (m_resolverThread.joinable()) {
            m_resolverThread.join();
        }
    }
    JST_LOG_ERROR("CVar system unavailable: {}", acceptedReason);
}

void CVarSystem::Stop() {
    std::vector<std::vector<IntReadCallback>> callbackDiscard;
    Cache discardedEntries;
#if defined(JST_UNIT_TESTS)
    RecordLifecycleAttemptForTest();
#endif
    {
        std::unique_lock lifecycleLock(m_lifecycleMutex);
#if defined(JST_UNIT_TESTS)
        RecordLifecycleEntryForTest();
#endif
        {
            std::unique_lock lock(m_mutex);
            if (m_state == CVarSystemState::Stopped) {
                return;
            }
            m_state = CVarSystemState::Stopping;
            m_resolverCv.notify_all();
            m_stateCv.wait(lock, [this] { return m_activeGameThreadPasses == 0; });
            FailAllCommandsLocked("CVar system stopped", callbackDiscard);
            discardedEntries.swap(m_cache);
            m_managedClaims.clear();
            m_needsInitialScan.clear();
            m_gameThreadReady = false;
            m_nextGeneration = 1;
        }
        if (m_resolverThread.joinable()) {
#if defined(JST_UNIT_TESTS)
            {
                std::lock_guard lock(m_mutex);
                m_testStopBeforeResolverJoin = true;
            }
            m_testResolverCv.notify_all();
#endif
            m_resolverThread.join();
        }
        {
            std::lock_guard lock(m_mutex);
            m_module.reset();
            m_state = CVarSystemState::Stopped;
            m_unavailableReason.clear();
        }
    }
}

} // namespace jst::core
