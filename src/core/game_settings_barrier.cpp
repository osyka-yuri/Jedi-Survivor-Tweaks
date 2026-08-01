#include "game_settings_barrier.hpp"

#include "atomic_activity_guard.hpp"
#include "cvar_layout.hpp"
#include "cvar_overrides.hpp"
#include "cvar_resolver.hpp"
#include "cvar_scanner.hpp"
#include "cvar_system.hpp"
#include "game_thread_dispatcher.hpp"
#include "hook_engine.hpp"
#include "logging.hpp"
#include "memory_scanner.hpp"
#include "pe_utils.hpp"

#include <array>
#include <format>
#include <string_view>

namespace jst::core {

namespace {

constexpr std::wstring_view kMaxFPSName = L"t.MaxFPS";
constexpr std::string_view kHookGroup = "Core.GameSettingsBarrier";
constexpr std::string_view kStringHookName =
    "Core.GameSettingsBarrier.SetString";
constexpr std::string_view kFloatHookName =
    "Core.GameSettingsBarrier.SetFloat";
constexpr std::string_view kIntHookName =
    "Core.GameSettingsBarrier.SetInt";

struct SetterTargets {
    uintptr_t object = 0;
    uintptr_t stringSetterRva = 0;
    uintptr_t floatSetterRva = 0;
    uintptr_t intSetterRva = 0;
};

[[nodiscard]] std::expected<void, std::string> UnregisterHooks(
    HookEngine& hooks) {
    std::string firstError;
    for (const std::string_view name : {
             kStringHookName,
             kFloatHookName,
             kIntHookName}) {
        if (auto removed = hooks.UnregisterHook(name); !removed && firstError.empty()) {
            firstError = std::format(
                "{} [{}]",
                removed.error().message,
                name);
        }
    }
    if (!firstError.empty()) {
        return std::unexpected(std::move(firstError));
    }
    return {};
}

[[nodiscard]] std::expected<SetterTargets, std::string> ResolveSetterTargets() {
    const auto module = GetGameModuleInfo();
    if (!module || !module->base || !module->size) {
        return std::unexpected("game module is unavailable");
    }

    constexpr std::array<std::wstring_view, 1> names{kMaxFPSName};
    const auto scanned = ScanForNames(names, *module);
    if (scanned.size() != 1) {
        return std::unexpected("t.MaxFPS name scan did not return one entry");
    }
    const auto resolved = ResolveFromScan(
        scanned.front(),
        *module,
        FindCVarOverride(kMaxFPSName));
    if (!resolved || !ValidateResolvedCVar(*resolved)) {
        return std::unexpected("t.MaxFPS IConsoleVariable is not registered yet");
    }

    const uintptr_t vtable = utils::SafeReadPointer(resolved->cvarObject);
    const auto readSetterRva = [&](size_t slot) -> uintptr_t {
        const uintptr_t setter = utils::SafeReadPointer(
            vtable + slot * sizeof(uintptr_t));
        if (setter < module->base || setter - module->base >= module->size ||
            !utils::IsExecutablePointer(setter)) {
            return 0;
        }
        return setter - module->base;
    };

    const uintptr_t stringSetterRva =
        readSetterRva(cvar_layout::kVtableSetString);
    const uintptr_t floatSetterRva =
        readSetterRva(cvar_layout::kVtableSetFloat);
    const uintptr_t intSetterRva =
        readSetterRva(cvar_layout::kVtableSetInt);
    if (!stringSetterRva || !floatSetterRva || !intSetterRva) {
        return std::unexpected("one or more t.MaxFPS setter overloads are invalid");
    }
    if (stringSetterRva == floatSetterRva ||
        stringSetterRva == intSetterRva ||
        floatSetterRva == intSetterRva) {
        return std::unexpected("t.MaxFPS setter overloads alias one address");
    }

    return SetterTargets{
        .object = resolved->cvarObject,
        .stringSetterRva = stringSetterRva,
        .floatSetterRva = floatSetterRva,
        .intSetterRva = intSetterRva,
    };
}

} // namespace

GameSettingsBarrier& GameSettingsBarrier::Instance() {
    static GameSettingsBarrier instance;
    return instance;
}

std::expected<void, std::string> GameSettingsBarrier::RegisterHooks(
    HookEngine& hooks) {
    if (State() != GameSettingsBarrierState::Stopped) {
        return std::unexpected("game-settings barrier is not stopped");
    }
    {
        std::lock_guard lock(m_mutex);
        m_unavailableReason.clear();
    }

    const auto targets = ResolveSetterTargets();
    if (!targets) {
        return std::unexpected(targets.error());
    }
    if (auto opened = m_detourGate.Open(); !opened) {
        return std::unexpected(opened.error());
    }

    struct Binding {
        std::string_view name;
        uintptr_t rva;
        uintptr_t detour;
    };
    const std::array bindings{
        Binding{
            .name = kStringHookName,
            .rva = targets->stringSetterRva,
            .detour = reinterpret_cast<uintptr_t>(&StringSetterDetour),
        },
        Binding{
            .name = kFloatHookName,
            .rva = targets->floatSetterRva,
            .detour = reinterpret_cast<uintptr_t>(&FloatSetterDetour),
        },
        Binding{
            .name = kIntHookName,
            .rva = targets->intSetterRva,
            .detour = reinterpret_cast<uintptr_t>(&IntSetterDetour),
        },
    };

    for (const auto& binding : bindings) {
        auto registered = hooks.RegisterAddressHook(
            HookSiteSpec{
                .name = std::string(binding.name),
                .group = std::string(kHookGroup),
                .minimumOverwriteLength = 5,
                .continuation = HookContinuation::ReplayOriginal,
                .detourGate = &m_detourGate,
            },
            binding.rva,
            binding.detour);
        if (!registered) {
            const std::string error = std::format(
                "{} [{}]",
                registered.error().message,
                binding.name);
            m_detourGate.Close();
            (void)UnregisterHooks(hooks);
            m_detourGate.WaitForIdle();
            return std::unexpected(error);
        }
    }

    m_maxFPSObject.store(targets->object, std::memory_order_release);
    m_state.store(GameSettingsBarrierState::Resolving, std::memory_order_release);
    JST_LOG_INFO(
        "Registered game-settings barrier on t.MaxFPS setters | object=0x{:X}.",
        targets->object);
    return {};
}

std::expected<void, std::string> GameSettingsBarrier::FinalizeResolution(
    HookEngine& hooks) {
    if (State() != GameSettingsBarrierState::Resolving) {
        return std::unexpected("game-settings barrier is not awaiting resolution");
    }

    const auto stringContinuation = hooks.GetContinuationAddress(kStringHookName);
    const auto floatContinuation = hooks.GetContinuationAddress(kFloatHookName);
    const auto intContinuation = hooks.GetContinuationAddress(kIntHookName);
    if (!stringContinuation || !*stringContinuation ||
        !floatContinuation || !*floatContinuation ||
        !intContinuation || !*intContinuation) {
        return std::unexpected("game-settings setter trampolines are unavailable");
    }

    m_originalStringSetter.store(
        reinterpret_cast<StringSetterFn>(*stringContinuation),
        std::memory_order_release);
    m_originalFloatSetter.store(
        reinterpret_cast<FloatSetterFn>(*floatContinuation),
        std::memory_order_release);
    m_originalIntSetter.store(
        reinterpret_cast<IntSetterFn>(*intContinuation),
        std::memory_order_release);
    return {};
}

std::expected<void, std::string> GameSettingsBarrier::FinalizeInstallation(
    HookEngine& hooks) {
    if (!m_originalStringSetter.load(std::memory_order_acquire) ||
        !m_originalFloatSetter.load(std::memory_order_acquire) ||
        !m_originalIntSetter.load(std::memory_order_acquire)) {
        return std::unexpected("game-settings originals were not bound");
    }
    if (!hooks.IsGroupInstalled(kHookGroup)) {
        return std::unexpected("game-settings setter hook group was not installed");
    }

    m_state.store(
        GameSettingsBarrierState::WaitingForPostStartupSetter,
        std::memory_order_release);
    JST_LOG_INFO(
        "Game-settings barrier installed; waiting for the post-startup "
        "t.MaxFPS SetByScalability setter.");
    return {};
}

void GameSettingsBarrier::MarkUnavailable(std::string reason) {
    m_maxFPSObject.store(0, std::memory_order_release);
    {
        std::lock_guard lock(m_mutex);
        m_unavailableReason = std::move(reason);
    }
    m_state.store(GameSettingsBarrierState::Unavailable, std::memory_order_release);
    JST_LOG_ERROR(
        "Game-settings barrier unavailable: {}",
        UnavailableReason());
}

std::string GameSettingsBarrier::UnavailableReason() const {
    std::lock_guard lock(m_mutex);
    return m_unavailableReason;
}

std::expected<void, std::string>
GameSettingsBarrier::ShutdownHooks(HookEngine& hooks) {
    m_state.exchange(
        GameSettingsBarrierState::Stopping,
        std::memory_order_acq_rel);
    m_maxFPSObject.store(0, std::memory_order_release);
    m_detourGate.Close();
    // Unregistration is idempotent. Always perform it so rollback remains
    // complete even if a preceding failure occurred between hook insertion
    // and lifecycle-state publication.
    const auto removed = UnregisterHooks(hooks);
    m_detourGate.WaitForIdle();
    for (uint32_t active = m_activeDetours.load(std::memory_order_acquire);
         active != 0;
         active = m_activeDetours.load(std::memory_order_acquire)) {
        m_activeDetours.wait(active, std::memory_order_acquire);
    }
    // Continuations remain process-lifetime allocations, matching the Tick
    // dispatcher shutdown contract.
    m_state.store(GameSettingsBarrierState::Stopped, std::memory_order_release);
    if (!removed) {
        return std::unexpected(removed.error());
    }
    return {};
}

void GameSettingsBarrier::ObserveSetterCompletion(
    bool onGameThread,
    uint32_t setBy) noexcept {
    if (!onGameThread ||
        (setBy & cvar_layout::kSetByMask) !=
            cvar_layout::kSetByScalability ||
        State() != GameSettingsBarrierState::WaitingForPostStartupSetter) {
        return;
    }

    GameSettingsBarrierState expected =
        GameSettingsBarrierState::WaitingForPostStartupSetter;
    if (!m_state.compare_exchange_strong(
            expected,
            GameSettingsBarrierState::Ready,
            std::memory_order_acq_rel)) {
        return;
    }

    try {
        if (!CVarSystem::Instance().OpenGameSettingsBarrier()) {
            MarkUnavailable("CVar system rejected the game-settings barrier");
            return;
        }
    } catch (...) {
        m_maxFPSObject.store(0, std::memory_order_release);
        m_state.store(
            GameSettingsBarrierState::Unavailable,
            std::memory_order_release);
        return;
    }
    try {
        JST_LOG_INFO(
            "Game-settings startup barrier opened after the post-startup "
            "t.MaxFPS SetByScalability setter.");
    } catch (...) {
        // Readiness is already published. Logging must not invalidate it.
    }
}

void __fastcall GameSettingsBarrier::StringSetterDetour(
    uintptr_t object,
    const wchar_t* value,
    uint32_t setBy) noexcept {
    auto& barrier = Instance();
    const AtomicActivityGuard detourGuard(barrier.m_activeDetours);
    const auto original =
        barrier.m_originalStringSetter.load(std::memory_order_acquire);
    if (original) {
        original(object, value, setBy);
        if (object == barrier.m_maxFPSObject.load(std::memory_order_acquire)) {
            barrier.ObserveSetterCompletion(
                GameThreadDispatcher::Instance().IsCurrentGameThread(),
                setBy);
        }
    }
}

void __fastcall GameSettingsBarrier::FloatSetterDetour(
    uintptr_t object,
    float value,
    uint32_t setBy) noexcept {
    auto& barrier = Instance();
    const AtomicActivityGuard detourGuard(barrier.m_activeDetours);
    const auto original =
        barrier.m_originalFloatSetter.load(std::memory_order_acquire);
    if (original) {
        original(object, value, setBy);
        if (object == barrier.m_maxFPSObject.load(std::memory_order_acquire)) {
            barrier.ObserveSetterCompletion(
                GameThreadDispatcher::Instance().IsCurrentGameThread(),
                setBy);
        }
    }
}

void __fastcall GameSettingsBarrier::IntSetterDetour(
    uintptr_t object,
    int32_t value,
    uint32_t setBy) noexcept {
    auto& barrier = Instance();
    const AtomicActivityGuard detourGuard(barrier.m_activeDetours);
    const auto original =
        barrier.m_originalIntSetter.load(std::memory_order_acquire);
    if (original) {
        original(object, value, setBy);
        if (object == barrier.m_maxFPSObject.load(std::memory_order_acquire)) {
            barrier.ObserveSetterCompletion(
                GameThreadDispatcher::Instance().IsCurrentGameThread(),
                setBy);
        }
    }
}

} // namespace jst::core
