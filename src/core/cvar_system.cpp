#include "cvar_system.hpp"
#include "cvar_overrides.hpp"
#include "cvar_resolver.hpp"
#include "cvar_scanner.hpp"
#include "logging.hpp"
#include "memory_scanner.hpp"
#include "pe_utils.hpp"
#include "string_utils.hpp"

#include <chrono>
#include <type_traits>

namespace jst::core {

namespace {
    constexpr std::chrono::seconds kPendingTimeout{30};

    template <typename T>
    constexpr const char* TypeName() {
        if constexpr (std::is_same_v<T, int32_t>) return "int";
        else return "float";
    }
} // anonymous namespace

CVarSystem& CVarSystem::Instance() {
    static CVarSystem instance;
    return instance;
}

CVarSystem::~CVarSystem() {
    StopPump();
}

const ModuleInfo* CVarSystem::GetOrFetchMod() {
    std::call_once(m_modOnce, [this] { m_mod = GetGameModuleInfo(); });
    return m_mod ? &*m_mod : nullptr;
}

template <typename T>
bool CVarSystem::WriteValue(const ResolvedCVar& rc, T value) {
    if (rc.writeAddrShadow == rc.writeAddr + sizeof(T)) {
        return utils::SafeWritePair<T>(rc.writeAddr, value);
    }
    if (!utils::SafeWrite<T>(rc.writeAddr, value)) return false;
    if (rc.writeAddrShadow != 0) {
        utils::SafeWrite<T>(rc.writeAddrShadow, value);
    }
    return true;
}

void CVarSystem::UpdateFlags(const ResolvedCVar& rc, std::wstring_view name) {
    if (!rc.cvarObject || !utils::IsValidPointer(rc.cvarObject)) return;

    const uintptr_t flagsAddr   = rc.cvarObject + cvar_layout::kFlagsOffset;
    const uint32_t currentFlags = utils::SafeReadInt32(flagsAddr);
    const uint32_t newFlags     = (currentFlags & cvar_layout::kMaxValidFlags) | cvar_layout::kSetByConsole;
    if (utils::SafeWrite<uint32_t>(flagsAddr, newFlags)) {
        JST_LOG_DEBUG("Updated priority flags for '{}' at 0x{:X}: 0x{:08X} -> 0x{:08X}.",
                     utils::WideToUtf8(name), flagsAddr, currentFlags, newFlags);
    }
}

template <typename T>
bool CVarSystem::WriteResolved(const ResolvedCVar& rc, std::wstring_view name, T value) {
    const T oldVal = std::is_same_v<T, int32_t>
        ? static_cast<T>(utils::SafeReadInt32(rc.writeAddr))
        : static_cast<T>(utils::SafeReadFloat(rc.writeAddr));

    if (!WriteValue<T>(rc, value)) return false;

    JST_LOG_INFO("Wrote {} for '{}' {} -> {}.",
                 TypeName<T>(), utils::WideToUtf8(name), oldVal, value);

    UpdateFlags(rc, name);
    return true;
}

template <typename T>
bool CVarSystem::SetTyped(std::wstring_view name, T value) {
    std::lock_guard lock(m_mutex);

    // Hot path: heterogeneous find — no wstring allocation for already-known CVars.
    if (auto it = m_cache.find(name); it != m_cache.end()) {
        auto& s = it->second.state;
        if (auto* rs = std::get_if<ResolvedState>(&s)) {
            return WriteResolved<T>(rs->rc, name, value);
        }
        if (auto* ps = std::get_if<PendingState>(&s)) {
            // Update the target value; the pump will apply it once resolved.
            ps->targetValue = value;
            return false;
        }
        // Defensive: unexpected monostate — fall through to cold path.
    }

    // Cold path: first request for this CVar.
    PendingState ps;
    ps.targetValue = value;
    ps.firstSeen   = std::chrono::steady_clock::now();
    m_cache.emplace(std::wstring(name), CVarState{std::move(ps)});

    ++m_pendingCount;
    m_needsInitialScan.emplace_back(name);  // wstring_view → wstring (one alloc)
    m_pumpCv.notify_all();

    JST_LOG_DEBUG("CVar '{}' queued for async init.", utils::WideToUtf8(name));
    return false;
}

bool CVarSystem::SetInt(std::wstring_view name, int32_t value) {
    return SetTyped<int32_t>(name, value);
}

bool CVarSystem::SetFloat(std::wstring_view name, float value) {
    return SetTyped<float>(name, value);
}

void CVarSystem::PerformInitialScan() {
    std::vector<std::wstring> namesToScan;
    {
        std::lock_guard lock(m_mutex);
        if (m_needsInitialScan.empty()) return;
        namesToScan = std::move(m_needsInitialScan);
    }

    const auto* mod = GetOrFetchMod();
    if (!mod || mod->base == 0) return;

    std::vector<std::wstring_view> nameViews;
    nameViews.reserve(namesToScan.size());
    for (const auto& n : namesToScan) nameViews.push_back(n);

    auto scannedEntries = ScanForNames(nameViews, *mod);

    std::lock_guard lock(m_mutex);

    for (auto& scanned : scannedEntries) {
        // scanned.name is std::wstring — use it directly for heterogeneous lookup.
        auto it = m_cache.find(scanned.name);
        if (it == m_cache.end()) continue;
        auto* ps = std::get_if<PendingState>(&it->second.state);
        if (!ps) continue;
        ps->scanData = std::move(scanned);  // move to avoid copying the candidate vectors
    }

    // Drop any CVars that weren't found in the binary.
    for (const auto& n : namesToScan) {
        auto it = m_cache.find(n);
        if (it == m_cache.end()) continue;
        auto* ps = std::get_if<PendingState>(&it->second.state);
        if (ps && ps->scanData.strAddr == 0) {
            JST_LOG_WARNING("CVar '{}' not found in game binary. Dropped.", utils::WideToUtf8(n));
            m_cache.erase(it);
            --m_pendingCount;
        }
    }
}

void CVarSystem::PumpLoop(std::chrono::milliseconds period) {
    while (true) {
        // Two-mode wait:
        //   • pending CVars exist → timed wait (periodic retry for object construction)
        //   • nothing pending    → indefinite wait (no spurious wakeups)
        {
            std::unique_lock lock(m_mutex);
            if (m_pendingCount > 0 || !m_needsInitialScan.empty()) {
                m_pumpCv.wait_for(lock, period, [this] {
                    return m_pumpStopRequested || !m_needsInitialScan.empty();
                });
            } else {
                m_pumpCv.wait(lock, [this] {
                    return m_pumpStopRequested ||
                           !m_needsInitialScan.empty() ||
                           m_pendingCount > 0;
                });
            }
            if (m_pumpStopRequested) break;
        }

        // Phase 1: Heavy .rdata/.text scan for newly-requested CVars.
        PerformInitialScan();

        // Phase 2: Evaluate pending CVars (unlock-resolve-relock).
        const auto* mod = GetOrFetchMod();
        if (!mod || mod->base == 0) continue;

        // ---- 2a: Snapshot pending work (locked, cheap) ----
        // We copy the scan data so Phase 2b can run without holding m_mutex
        // while calling VirtualProtect / Logger::Log (which have their own locks).
        struct Snapshot {
            std::wstring   name;
            ScanEntry      scanData;
            CVarValue      targetValue;
            const CVarOverride* override;
            std::chrono::steady_clock::time_point firstSeen;
        };
        std::vector<Snapshot> snapshots;
        {
            std::lock_guard lock(m_mutex);
            for (auto& [name, cvarState] : m_cache) {
                const auto* ps = std::get_if<PendingState>(&cvarState.state);
                if (!ps || ps->scanData.strAddr == 0) continue;
                snapshots.push_back({
                    name,
                    ps->scanData,                              // copy candidate vectors
                    ps->targetValue,
                    CVarOverrideTable::Instance().Find(name),
                    ps->firstSeen
                });
            }
        }

        if (snapshots.empty()) continue;

        // ---- 2b: Resolve and write — no mutex held ----
        // VirtualProtect and file I/O must not run under m_mutex.
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::pair<std::wstring, ResolvedCVar>> resolved;
        std::vector<std::wstring> timedOut;

        for (auto& snap : snapshots) {
            auto result = ResolveFromScan(snap.scanData, *mod, snap.override);
            if (result) {
                std::visit([&](auto&& val) {
                    using T = std::decay_t<decltype(val)>;
                    WriteResolved<T>(*result, snap.name, val);
                }, snap.targetValue);
                resolved.emplace_back(snap.name, *result);
            } else if (now - snap.firstSeen > kPendingTimeout) {
                JST_LOG_WARNING("Deferred init for '{}' timed out.", utils::WideToUtf8(snap.name));
                timedOut.push_back(std::move(snap.name));
            }
        }

        // ---- 2c: Commit results (locked, cheap) ----
        {
            std::lock_guard lock(m_mutex);
            for (auto& [name, rc] : resolved) {
                auto it = m_cache.find(name);
                if (it != m_cache.end() && std::holds_alternative<PendingState>(it->second.state)) {
                    JST_LOG_DEBUG("Deferred init successful for '{}'.", utils::WideToUtf8(name));
                    it->second.state = ResolvedState{rc};
                    --m_pendingCount;
                }
            }
            for (const auto& name : timedOut) {
                if (m_cache.erase(name)) --m_pendingCount;
            }
        }
    }
}

void CVarSystem::StartPump(std::chrono::milliseconds period) {
    std::lock_guard lock(m_mutex);
    if (m_pumpRunning) return;
    m_pumpRunning = true;
    m_pumpStopRequested = false;
    m_pumpThread = std::thread([this, period] { PumpLoop(period); });
}

void CVarSystem::StopPump() {
    {
        std::lock_guard lock(m_mutex);
        if (!m_pumpRunning) return;
        m_pumpStopRequested = true;
    }
    m_pumpCv.notify_all();
    if (m_pumpThread.joinable()) m_pumpThread.join();

    std::lock_guard lock(m_mutex);
    m_pumpRunning = false;
}

} // namespace jst::core
