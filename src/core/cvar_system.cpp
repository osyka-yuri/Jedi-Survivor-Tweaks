#include "cvar_system.hpp"

#include "cvar_layout.hpp"
#include "cvar_overrides.hpp"
#include "cvar_watch_registry.hpp"
#include "logging.hpp"
#include "memory_scanner.hpp"
#include "pe_utils.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace jst::core {

namespace {

template <typename T>
constexpr const char* TypeName() noexcept {
    if constexpr (std::is_same_v<T, int32_t>) {
        return "int";
    } else {
        return "float";
    }
}

} // namespace

CVarSystem::CVarSystem()
    : m_watches(std::make_unique<CVarWatchRegistry>()) {}

CVarSystem::~CVarSystem() {
    StopPump();
}

CVarSystem& CVarSystem::Instance() {
    static CVarSystem instance;
    return instance;
}

CVarWatchSubscription::~CVarWatchSubscription() {
    Reset();
}

CVarWatchSubscription::CVarWatchSubscription(CVarWatchSubscription&& other) noexcept
    : m_owner(std::exchange(other.m_owner, nullptr)),
      m_id(std::exchange(other.m_id, 0)) {}

CVarWatchSubscription& CVarWatchSubscription::operator=(
    CVarWatchSubscription&& other) noexcept {
    if (this != &other) {
        Reset();
        m_owner = std::exchange(other.m_owner, nullptr);
        m_id = std::exchange(other.m_id, 0);
    }
    return *this;
}

void CVarWatchSubscription::Reset() {
    if (m_owner) {
        m_owner->CancelWatch(m_id);
    }
    m_owner = nullptr;
    m_id = 0;
}

const ModuleInfo* CVarSystem::GetOrFetchModule() {
    std::lock_guard lock(m_moduleMutex);
    if (!m_module) {
        m_module = GetGameModuleInfo();
    }
    return m_module ? &*m_module : nullptr;
}

template <typename T>
bool CVarSystem::WriteValue(const ResolvedCVar& resolved, T value) {
    if (resolved.writeAddrShadow == resolved.writeAddr + sizeof(T)) {
        return utils::SafeWritePair<T>(resolved.writeAddr, value);
    }
    if (!utils::SafeWrite<T>(resolved.writeAddr, value)) {
        return false;
    }
    if (resolved.writeAddrShadow != 0 &&
        !utils::SafeWrite<T>(resolved.writeAddrShadow, value)) {
        return false;
    }
    return true;
}

void CVarSystem::UpdateFlags(const ResolvedCVar& resolved, std::wstring_view name) {
    if (!resolved.cvarObject || !utils::IsValidPointer(resolved.cvarObject)) {
        return;
    }

    const uintptr_t flagsAddress = resolved.cvarObject + cvar_layout::kFlagsOffset;
    const uint32_t currentFlags = utils::SafeReadInt32(flagsAddress);
    const uint32_t newFlags =
        (currentFlags & cvar_layout::kMaxValidFlags) | cvar_layout::kSetByConsole;
    if (utils::SafeWrite<uint32_t>(flagsAddress, newFlags)) {
        JST_LOG_DEBUG("Updated priority flags for '{}' at 0x{:X}: 0x{:08X} -> 0x{:08X}.",
                      utils::WideToUtf8(name), flagsAddress, currentFlags, newFlags);
    }
}

template <typename T>
bool CVarSystem::WriteResolved(
    const ResolvedCVar& resolved, std::wstring_view name, T value) {
    const T oldValue = std::is_same_v<T, int32_t>
        ? static_cast<T>(utils::SafeReadInt32(resolved.writeAddr))
        : static_cast<T>(utils::SafeReadFloat(resolved.writeAddr));

    if (!WriteValue<T>(resolved, value)) {
        return false;
    }

    JST_LOG_INFO("Wrote {} for '{}' {} -> {}.",
                 TypeName<T>(), utils::WideToUtf8(name), oldValue, value);
    UpdateFlags(resolved, name);
    return true;
}

void CVarSystem::EnqueuePendingLocked(
    std::wstring_view name, std::optional<CVarValue> target) {
    PendingState pending;
    pending.targetValue = std::move(target);
    pending.firstSeen = std::chrono::steady_clock::now();
    m_cache.emplace(std::wstring(name), std::move(pending));
    m_needsInitialScan.emplace_back(name);
    m_pumpCv.notify_all();
}

bool CVarSystem::HasPendingLocked() const {
    return std::ranges::any_of(m_cache, [](const auto& item) {
        if (std::holds_alternative<PendingState>(item.second)) {
            return true;
        }
        const auto& resolved = std::get<ResolvedEntryPtr>(item.second);
        std::lock_guard valueLock(resolved->valueMutex);
        return resolved->pendingWrite.has_value();
    });
}

template <typename T>
bool CVarSystem::SetTyped(std::wstring_view name, T value) {
    std::unique_lock cacheLock(m_mutex);
    if (auto found = m_cache.find(name); found != m_cache.end()) {
        if (auto* pending = std::get_if<PendingState>(&found->second)) {
            pending->targetValue = CVarValue{value};
            return false;
        }

        const auto resolved = std::get<ResolvedEntryPtr>(found->second);
        std::unique_lock valueLock(resolved->valueMutex);
        cacheLock.unlock();
        resolved->pendingWrite = CVarValue{value};
        resolved->pendingWriteSince = std::chrono::steady_clock::now();
        if (WriteResolved<T>(resolved->resolved, name, value)) {
            resolved->pendingWrite.reset();
            return true;
        }
        valueLock.unlock();
        m_pumpCv.notify_all();
        JST_LOG_WARNING("CVar write for '{}' failed and was queued for retry.",
                        utils::WideToUtf8(name));
        return false;
    }

    EnqueuePendingLocked(name, CVarValue{value});
    JST_LOG_DEBUG("CVar '{}' queued for async init.", utils::WideToUtf8(name));
    return false;
}

bool CVarSystem::SetInt(std::wstring_view name, int32_t value) {
    return SetTyped(name, value);
}

bool CVarSystem::SetFloat(std::wstring_view name, float value) {
    return SetTyped(name, value);
}

CVarWatchSubscription CVarSystem::WatchInt(IntWatchRequest request) {
    if (request.name.empty() || !request.onValue) {
        return {};
    }

    const std::wstring name = request.name;
    const uint64_t id = m_watches->Register(std::move(request));
    {
        std::lock_guard lock(m_mutex);
        if (!m_cache.contains(name)) {
            EnqueuePendingLocked(name, std::nullopt);
        }
    }
    m_pumpCv.notify_all();
    JST_LOG_DEBUG("CVar watch #{} registered for '{}'.", id, utils::WideToUtf8(name));
    return CVarWatchSubscription(this, id);
}

void CVarSystem::CancelWatch(uint64_t id) {
    m_watches->Cancel(id);
    JST_LOG_DEBUG("CVar watch #{} cancelled.", id);
}

std::optional<int32_t> CVarSystem::ReadResolvedInt(std::wstring_view name) const {
    ResolvedEntryPtr entry;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_cache.find(name);
        if (found == m_cache.end()) {
            return std::nullopt;
        }
        const auto* resolved = std::get_if<ResolvedEntryPtr>(&found->second);
        if (!resolved) {
            return std::nullopt;
        }
        entry = *resolved;
    }

    std::lock_guard valueLock(entry->valueMutex);
    return utils::SafeReadInt32(entry->resolved.writeAddr);
}

void CVarSystem::RetryPendingWrites() {
    struct Snapshot {
        std::wstring name;
        ResolvedEntryPtr entry;
    };

    std::vector<Snapshot> snapshots;
    std::chrono::milliseconds timeout;
    {
        std::lock_guard lock(m_mutex);
        timeout = m_pendingTimeout;
        for (const auto& [name, state] : m_cache) {
            const auto* resolved = std::get_if<ResolvedEntryPtr>(&state);
            if (resolved) {
                snapshots.push_back(Snapshot{.name = name, .entry = *resolved});
            }
        }
    }

    const auto now = std::chrono::steady_clock::now();
    for (const auto& snapshot : snapshots) {
        std::unique_lock valueLock(snapshot.entry->valueMutex);
        if (!snapshot.entry->pendingWrite) {
            continue;
        }
        if (now - snapshot.entry->pendingWriteSince > timeout) {
            snapshot.entry->pendingWrite.reset();
            valueLock.unlock();
            JST_LOG_WARNING("Deferred write for '{}' timed out.",
                            utils::WideToUtf8(snapshot.name));
            continue;
        }

        bool writeSucceeded = false;
        std::visit([&](auto value) {
            writeSucceeded = WriteResolved(
                snapshot.entry->resolved, snapshot.name, value);
        }, *snapshot.entry->pendingWrite);
        if (writeSucceeded) {
            snapshot.entry->pendingWrite.reset();
        }
    }
}

void CVarSystem::PerformInitialScan() {
    std::vector<std::wstring> names;
    {
        std::lock_guard lock(m_mutex);
        if (m_needsInitialScan.empty()) {
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
        if (found == m_cache.end()) {
            continue;
        }
        if (auto* pending = std::get_if<PendingState>(&found->second)) {
            pending->scanData = std::move(result);
        }
    }

    for (const auto& name : names) {
        const auto found = m_cache.find(name);
        if (found == m_cache.end()) {
            continue;
        }
        const auto* pending = std::get_if<PendingState>(&found->second);
        if (pending && pending->scanData.strAddr == 0) {
            JST_LOG_WARNING("CVar '{}' not found in game binary. Dropped.",
                            utils::WideToUtf8(name));
            m_cache.erase(found);
        }
    }
}

void CVarSystem::ResolvePendingCVars(const ModuleInfo& module) {
    struct Snapshot {
        std::wstring name;
        ScanEntry scanData;
        const CVarOverride* overrideEntry = nullptr;
        std::chrono::steady_clock::time_point firstSeen;
    };

    std::vector<Snapshot> snapshots;
    std::chrono::milliseconds timeout;
    {
        std::lock_guard lock(m_mutex);
        timeout = m_pendingTimeout;
        for (const auto& [name, state] : m_cache) {
            const auto* pending = std::get_if<PendingState>(&state);
            if (!pending || pending->scanData.strAddr == 0) {
                continue;
            }
            snapshots.push_back(Snapshot{
                .name = name,
                .scanData = pending->scanData,
                .overrideEntry = CVarOverrideTable::Instance().Find(name),
                .firstSeen = pending->firstSeen,
            });
        }
    }

    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<std::wstring, ResolvedCVar>> resolved;
    std::vector<std::wstring> expired;
    for (auto& snapshot : snapshots) {
        auto result = ResolveFromScan(
            snapshot.scanData, module, snapshot.overrideEntry);
        if (result) {
            resolved.emplace_back(std::move(snapshot.name), *result);
        } else if (now - snapshot.firstSeen > timeout) {
            expired.push_back(std::move(snapshot.name));
        }
    }

    for (const auto& [name, value] : resolved) {
        (void)CommitResolved(name, value);
    }

    std::vector<std::wstring> removed;
    {
        std::lock_guard lock(m_mutex);
        for (auto& name : expired) {
            if (m_watches->HasFor(name)) {
                continue;
            }
            if (m_cache.erase(name) != 0) {
                removed.push_back(std::move(name));
            }
        }
    }
    for (const auto& name : removed) {
        JST_LOG_WARNING("Deferred init for '{}' timed out.", utils::WideToUtf8(name));
    }
}

bool CVarSystem::CommitResolved(std::wstring_view name, const ResolvedCVar& resolved) {
    std::unique_lock cacheLock(m_mutex);
    const auto found = m_cache.find(name);
    if (found == m_cache.end()) {
        return false;
    }
    auto* pending = std::get_if<PendingState>(&found->second);
    if (!pending) {
        return false;
    }

    const auto target = pending->targetValue;
    auto entry = std::make_shared<ResolvedEntry>(resolved);
    std::unique_lock valueLock(entry->valueMutex);
    entry->pendingWrite = target;
    entry->pendingWriteSince = std::chrono::steady_clock::now();
    found->second = entry;
    cacheLock.unlock();

    bool writeSucceeded = true;
    if (target) {
        std::visit([&](auto value) {
            writeSucceeded = WriteResolved(entry->resolved, name, value);
        }, *target);
        if (writeSucceeded) {
            entry->pendingWrite.reset();
        }
    }

    if (writeSucceeded) {
        JST_LOG_DEBUG("Deferred init successful for '{}'.", utils::WideToUtf8(name));
    } else {
        JST_LOG_WARNING("Deferred resolve succeeded; initial write for '{}' will be retried.",
                        utils::WideToUtf8(name));
        valueLock.unlock();
        m_pumpCv.notify_all();
    }
    return true;
}

void CVarSystem::PumpLoop(std::chrono::milliseconds period) {
    while (true) {
        {
            std::unique_lock lock(m_mutex);
            const bool hasPeriodicWork =
                HasPendingLocked() || !m_needsInitialScan.empty() || m_watches->HasAny();
            if (hasPeriodicWork) {
                m_pumpCv.wait_for(lock, period, [this] { return m_pumpStopRequested; });
            } else {
                m_pumpCv.wait(lock, [this] {
                    return m_pumpStopRequested || HasPendingLocked() ||
                           !m_needsInitialScan.empty() || m_watches->HasAny();
                });
            }
            if (m_pumpStopRequested) {
                break;
            }
        }

        PerformInitialScan();
        RetryPendingWrites();
        if (const auto* module = GetOrFetchModule(); module && module->base != 0) {
            ResolvePendingCVars(*module);
        }
        m_watches->Evaluate([this](std::wstring_view name) {
            return ReadResolvedInt(name);
        });
    }
}

void CVarSystem::StartPump(std::chrono::milliseconds period) {
    std::lock_guard lock(m_mutex);
    if (m_pumpRunning) {
        return;
    }
    m_pumpRunning = true;
    m_pumpStopRequested = false;
    m_pumpThread = std::thread([this, period] { PumpLoop(period); });
}

void CVarSystem::StopPump() {
    bool shouldJoin = false;
    {
        std::lock_guard lock(m_mutex);
        if (m_pumpRunning) {
            m_pumpStopRequested = true;
            shouldJoin = true;
        }
    }

    if (shouldJoin) {
        m_pumpCv.notify_all();
        if (m_pumpThread.joinable()) {
            m_pumpThread.join();
        }
    }

    m_watches->Clear();
    {
        std::lock_guard lock(m_mutex);
        m_pumpRunning = false;
        m_pumpStopRequested = false;
    }
    m_pumpCv.notify_all();
}

} // namespace jst::core
