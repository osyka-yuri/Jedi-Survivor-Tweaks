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

CVarSystem::ResolveResult CVarSystem::Resolve(std::wstring_view name) {
    ResolveResult result;

    const auto cached = m_cache.find(name);
    if (cached != m_cache.end()) {
        result.cvar = cached->second;
        return result;
    }

    const auto* mod = GetOrFetchMod();
    if (!mod || mod->base == 0) return result;

    const auto* override = CVarOverrideTable::Instance().Find(name);
    if (auto rc = ResolveFromOverride(override, *mod)) {
        m_cache.emplace(std::wstring(name), *rc);
        result.cvar = std::move(*rc);
        return result;
    }

    std::wstring_view nameArr[] = {name};
    auto entries = ScanForNames(nameArr, *mod);
    if (entries.empty()) return result;

    auto scanResult = ResolveFromScan(entries[0], *mod, override);
    if (scanResult.cvar) {
        m_cache.emplace(std::wstring(name), *scanResult.cvar);
    }
    result.cvar = std::move(scanResult.cvar);
    result.pendingPtr = std::move(scanResult.pendingPtr);
    return result;
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
        JST_LOG_INFO("Updated priority flags for '{}' at 0x{:X}: 0x{:08X} -> 0x{:08X}.",
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

    auto result = Resolve(name);
    if (result.cvar) {
        return WriteResolved<T>(*result.cvar, name, value);
    }
    if (result.pendingPtr) {
        JST_LOG_INFO("'{}' is not ready, queued for deferred application.",
                     utils::WideToUtf8(name));
        PendingEntry entry;
        entry.name = std::wstring(name);
        entry.value = value;
        entry.addedAt = std::chrono::steady_clock::now();
        m_pending.push_back(std::move(entry));
        m_pumpCv.notify_all();
        return true;
    }
    JST_LOG_WARNING("CVar '{}' not found in game binary (type: {}).",
                    utils::WideToUtf8(name), TypeName<T>());
    return false;
}

bool CVarSystem::SetInt(std::wstring_view name, int32_t value) {
    return SetTyped<int32_t>(name, value);
}

bool CVarSystem::SetFloat(std::wstring_view name, float value) {
    return SetTyped<float>(name, value);
}

void CVarSystem::ResolveBatch(std::span<const std::wstring_view> names) {
    if (names.empty()) return;

    const auto* mod = GetOrFetchMod();
    if (!mod || mod->base == 0) return;

    auto entries = ScanForNames(names, *mod);
    if (entries.empty()) return;

    std::lock_guard lock(m_mutex);
    for (const auto& entry : entries) {
        if (m_cache.find(entry.name) != m_cache.end()) continue;

        const auto* override = CVarOverrideTable::Instance().Find(entry.name);
        if (auto rc = ResolveFromOverride(override, *mod)) {
            m_cache.emplace(std::wstring(entry.name), *rc);
            JST_LOG_INFO("Resolved '{}' via override.", utils::WideToUtf8(entry.name));
            continue;
        }

        auto scanResult = ResolveFromScan(entry, *mod, override);
        if (scanResult.cvar) {
            m_cache.emplace(std::wstring(entry.name), *scanResult.cvar);
            JST_LOG_INFO("Resolved '{}'.", utils::WideToUtf8(entry.name));
        }
    }
}

void CVarSystem::ProcessPending() {
    if (m_pending.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    std::vector<PendingEntry> stillPending;
    stillPending.reserve(m_pending.size());

    for (auto& pending : m_pending) {
        auto result = Resolve(pending.name);
        if (result.cvar) {
            std::visit([&](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                WriteResolved<T>(*result.cvar, pending.name, value);
            }, pending.value);
            continue;
        }

        if (now - pending.addedAt > kPendingTimeout) {
            JST_LOG_WARNING("Deferred apply for '{}' timed out and was discarded.",
                            utils::WideToUtf8(pending.name));
        } else {
            stillPending.push_back(std::move(pending));
        }
    }

    m_pending = std::move(stillPending);
}

void CVarSystem::PumpLoop(std::chrono::milliseconds period) {
    std::unique_lock lock(m_mutex);
    while (!m_pumpStopRequested) {
        m_pumpCv.wait_for(lock, period, [this] { return m_pumpStopRequested; });
        if (m_pumpStopRequested) break;
        ProcessPending();
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
