#include "game_thread_dispatcher.hpp"

#include "atomic_activity_guard.hpp"
#include "hook_engine.hpp"
#include "logging.hpp"
#include "memory_scanner.hpp"
#include "pe_utils.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <utility>

namespace jst::core {

namespace {

constexpr std::string_view kHookName = "Core.FEngineLoop.Tick";
constexpr std::string_view kHookGroup = "Core.GameThreadDispatcher";

// Patch 9 encrypts .text on disk, while Windows x64 unwind metadata remains
// readable. This exact sequence starts with FEngineLoop::Tick and covers four
// following runtime functions. Multiple local Patch 9 crash contexts captured
// during active play identify the named GameThread inside the first, 0x1056-
// byte record immediately below GuardedMain. Shutdown captures instead pass
// through 0x00A52AE0; that function must never be mistaken for Tick.
constexpr std::array<uint8_t, 60> kTickRuntimeFunctionSignature{
    0x40, 0xBB, 0xA4, 0x00, // Tick begin:       0x00A4BB40
    0x96, 0xCB, 0xA4, 0x00, // Tick end:         0x00A4CB96
    0x0C, 0xFC, 0x04, 0x06, // Tick unwind
    0xA0, 0xCB, 0xA4, 0x00, // following function begin
    0xF1, 0xCB, 0xA4, 0x00, // following function end
    0x74, 0xF6, 0x04, 0x06, // following function unwind
    0x00, 0xCC, 0xA4, 0x00, // following function begin
    0x34, 0xCC, 0xA4, 0x00, // following function end
    0x74, 0xF6, 0x04, 0x06, // following function unwind
    0x40, 0xCC, 0xA4, 0x00, // following function begin
    0x86, 0xCC, 0xA4, 0x00, // following function end
    0xD4, 0xF6, 0x04, 0x06, // following function unwind
    0x90, 0xCC, 0xA4, 0x00, // following function begin
    0xDC, 0xCC, 0xA4, 0x00, // following function end
    0xB8, 0xF9, 0x04, 0x06, // following function unwind
};
constexpr uint32_t kSupportedTimestamp = 0x66DF3498;
constexpr uint32_t kSupportedImageSize = 0x0722A000;
constexpr uint32_t kExpectedTickRva = 0x00A4BB40;

[[nodiscard]] std::expected<uintptr_t, std::string>
ResolveFEngineLoopTickRva() {
    const auto module = GetGameModuleInfo();
    if (!module || !module->base ||
        module->size < sizeof(IMAGE_DOS_HEADER) ||
        module->size != kSupportedImageSize) {
        return std::unexpected(std::format(
            "unsupported JediSurvivor image size (expected 0x{:X})",
            kSupportedImageSize));
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module->base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return std::unexpected("invalid JediSurvivor DOS header");
    }
    const auto ntOffset = static_cast<size_t>(dos->e_lfanew);
    if (ntOffset > module->size - sizeof(IMAGE_NT_HEADERS)) {
        return std::unexpected("JediSurvivor NT headers are outside the image");
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        module->base + ntOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.TimeDateStamp != kSupportedTimestamp ||
        nt->OptionalHeader.SizeOfImage != kSupportedImageSize) {
        return std::unexpected(std::format(
            "unsupported JediSurvivor build (timestamp=0x{:X}, image=0x{:X})",
            nt->FileHeader.TimeDateStamp,
            nt->OptionalHeader.SizeOfImage));
    }

    const auto pdata = utils::GetModuleSection(*module, ".pdata");
    if (!pdata || pdata->size < kTickRuntimeFunctionSignature.size()) {
        return std::unexpected("JediSurvivor .pdata section is unavailable");
    }

    const uint8_t* uniqueMatch = nullptr;
    size_t matches = 0;
    for (size_t offset = 0;
         offset + kTickRuntimeFunctionSignature.size() <= pdata->size;
         ++offset) {
        const auto* candidate = pdata->base + offset;
        if (std::memcmp(
                candidate,
                kTickRuntimeFunctionSignature.data(),
                kTickRuntimeFunctionSignature.size()) != 0) {
            continue;
        }
        uniqueMatch = candidate;
        ++matches;
        if (matches > 1) {
            break;
        }
    }
    if (matches != 1 || !uniqueMatch) {
        return std::unexpected(std::format(
            "FEngineLoop::Tick unwind signature matched {} times",
            matches));
    }

    uint32_t targetRva = 0;
    std::memcpy(&targetRva, uniqueMatch, sizeof(targetRva));
    const uintptr_t target = module->base + targetRva;
    if (targetRva != kExpectedTickRva ||
        targetRva >= module->size ||
        !utils::IsExecutablePointer(target)) {
        return std::unexpected(std::format(
            "invalid FEngineLoop::Tick target RVA 0x{:X}",
            targetRva));
    }
    return static_cast<uintptr_t>(targetRva);
}

} // namespace

GameThreadDispatcher& GameThreadDispatcher::Instance() {
    static GameThreadDispatcher instance;
    return instance;
}

std::expected<void, std::string>
GameThreadDispatcher::RegisterHook(HookEngine& hooks) {
    if (State() != GameThreadDispatcherState::Stopped) {
        return std::unexpected("dispatcher is not stopped");
    }
    {
        std::lock_guard lock(m_mutex);
        m_unavailableReason.clear();
    }
    const auto targetRva = ResolveFEngineLoopTickRva();
    if (!targetRva) {
        return std::unexpected(targetRva.error());
    }
    if (auto opened = m_detourGate.Open(); !opened) {
        return std::unexpected(opened.error());
    }

    HookSiteSpec spec{
        .name = std::string(kHookName),
        .group = std::string(kHookGroup),
        // HookEngine reaches the nearby gateway with a 5-byte rel32 jump;
        // the gateway owns the absolute detour jump. Keep the Tick splice at
        // the smallest complete instruction window.
        .minimumOverwriteLength = 5,
        .continuation = HookContinuation::ReplayOriginal,
        .detourGate = &m_detourGate,
    };
    auto registered = hooks.RegisterAddressHook(
        std::move(spec),
        *targetRva,
        reinterpret_cast<uintptr_t>(&TickDetour));
    if (!registered) {
        m_detourGate.Close();
        m_detourGate.WaitForIdle();
        return std::unexpected(registered.error().message);
    }

    m_state.store(GameThreadDispatcherState::Resolving, std::memory_order_release);
    m_gameThreadId.store(0, std::memory_order_release);
    return {};
}

std::expected<void, std::string>
GameThreadDispatcher::FinalizeResolution(HookEngine& hooks) {
    if (State() != GameThreadDispatcherState::Resolving) {
        return std::unexpected("dispatcher hook is not awaiting resolution");
    }
    const auto continuation = hooks.GetContinuationAddress(kHookName);
    if (!continuation || *continuation == 0) {
        return std::unexpected("FEngineLoop::Tick original trampoline is unavailable");
    }
    m_originalTick.store(
        reinterpret_cast<EngineTickFn>(*continuation),
        std::memory_order_release);
    return {};
}

std::expected<void, std::string>
GameThreadDispatcher::FinalizeInstallation(HookEngine& hooks) {
    if (!m_originalTick.load(std::memory_order_acquire)) {
        return std::unexpected("dispatcher original trampoline was not bound");
    }
    if (!hooks.IsGroupInstalled(kHookGroup)) {
        return std::unexpected("FEngineLoop::Tick hook group was not installed");
    }
    m_state.store(
        GameThreadDispatcherState::WaitingForFirstTick,
        std::memory_order_release);
    JST_LOG_INFO("Game-thread dispatcher installed; waiting for first post-Tick barrier.");
    return {};
}

void GameThreadDispatcher::SetTickHandler(TickHandler handler) {
    std::lock_guard lock(m_mutex);
    m_tickHandler = std::move(handler);
}

bool GameThreadDispatcher::IsCurrentGameThread() const noexcept {
    const uint32_t gameThreadId = m_gameThreadId.load(std::memory_order_acquire);
    return IsReady() && gameThreadId != 0 &&
        gameThreadId == static_cast<uint32_t>(::GetCurrentThreadId());
}

void GameThreadDispatcher::MarkUnavailable(std::string reason) {
    m_gameThreadId.store(0, std::memory_order_release);
    {
        std::lock_guard lock(m_mutex);
        m_unavailableReason = std::move(reason);
        m_tickHandler = {};
    }
    m_state.store(GameThreadDispatcherState::Unavailable, std::memory_order_release);
    JST_LOG_ERROR("Game-thread dispatcher unavailable: {}", UnavailableReason());
}

std::string GameThreadDispatcher::UnavailableReason() const {
    std::lock_guard lock(m_mutex);
    return m_unavailableReason;
}

void GameThreadDispatcher::BeginShutdown() {
    const auto previous = m_state.exchange(
        GameThreadDispatcherState::Stopping,
        std::memory_order_acq_rel);
    if (previous == GameThreadDispatcherState::Stopped) {
        m_state.store(GameThreadDispatcherState::Stopped, std::memory_order_release);
        return;
    }

    {
        std::lock_guard lock(m_mutex);
        m_tickHandler = {};
    }
    for (uint32_t active = m_activeCallbacks.load(std::memory_order_acquire);
         active != 0;
         active = m_activeCallbacks.load(std::memory_order_acquire)) {
        m_activeCallbacks.wait(active, std::memory_order_acquire);
    }
}

std::expected<void, std::string>
GameThreadDispatcher::ShutdownHook(HookEngine& hooks) {
    BeginShutdown();
    m_detourGate.Close();
    const auto removed = hooks.UnregisterHook(kHookName);
    m_detourGate.WaitForIdle();
    for (uint32_t active = m_activeDetours.load(std::memory_order_acquire);
         active != 0;
         active = m_activeDetours.load(std::memory_order_acquire)) {
        m_activeDetours.wait(active, std::memory_order_acquire);
    }
    // Gateways are process-lifetime allocations. Retain the trampoline so a
    // detour already past the removed entry splice can still call original.
    m_gameThreadId.store(0, std::memory_order_release);
    m_state.store(GameThreadDispatcherState::Stopped, std::memory_order_release);
    if (!removed) {
        return std::unexpected(removed.error().message);
    }
    return {};
}

void GameThreadDispatcher::Stop() {
    BeginShutdown();
    m_gameThreadId.store(0, std::memory_order_release);
    m_state.store(GameThreadDispatcherState::Stopped, std::memory_order_release);
}

void __fastcall GameThreadDispatcher::TickDetour(void* engineLoop) noexcept {
    auto& dispatcher = Instance();
    const AtomicActivityGuard detourGuard(dispatcher.m_activeDetours);

    const auto original = dispatcher.m_originalTick.load(std::memory_order_acquire);
    if (original) {
        original(engineLoop);
        dispatcher.DispatchAfterOriginalTick();
    }

}

void GameThreadDispatcher::DispatchAfterOriginalTick() noexcept {
    auto state = State();
    if ((state == GameThreadDispatcherState::WaitingForFirstTick ||
         state == GameThreadDispatcherState::Ready) &&
        m_gameThreadId.load(std::memory_order_acquire) == 0) {
        uint32_t expected = 0;
        (void)m_gameThreadId.compare_exchange_strong(
            expected,
            static_cast<uint32_t>(::GetCurrentThreadId()),
            std::memory_order_acq_rel);
    }
    if (state == GameThreadDispatcherState::WaitingForFirstTick) {
        GameThreadDispatcherState expected =
            GameThreadDispatcherState::WaitingForFirstTick;
        if (m_state.compare_exchange_strong(
                expected,
                GameThreadDispatcherState::Ready,
                std::memory_order_acq_rel)) {
            JST_LOG_INFO("Game-thread startup barrier opened after FEngineLoop::Tick.");
        }
        state = State();
    }
    if (state != GameThreadDispatcherState::Ready) {
        return;
    }

    const AtomicActivityGuard callbackGuard(m_activeCallbacks);
    TickHandler handler;
    {
        std::lock_guard lock(m_mutex);
        if (State() != GameThreadDispatcherState::Ready || !m_tickHandler) {
            return;
        }
        handler = m_tickHandler;
    }

    try {
        handler();
    } catch (const std::exception& error) {
        JST_LOG_ERROR("Game-thread dispatcher callback threw: {}", error.what());
    } catch (...) {
        JST_LOG_ERROR("Game-thread dispatcher callback threw an unknown exception.");
    }
}

} // namespace jst::core
