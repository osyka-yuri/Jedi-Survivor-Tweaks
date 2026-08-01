#pragma once

#include "detour_gate.hpp"
#include "hook_types.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <string>

namespace jst::core {

class HookEngine;

enum class GameThreadDispatcherState : uint8_t {
    Stopped,
    Resolving,
    WaitingForFirstTick,
    Ready,
    Unavailable,
    Stopping,
};

/**
 * Executes engine-facing work after FEngineLoop::Tick returns on the game
 * thread. The first intercepted post-tick establishes the game-thread
 * identity; the CVar runtime owns its separate late-settings barrier.
 */
class GameThreadDispatcher final {
public:
    using TickHandler = std::function<void()>;

    [[nodiscard]] static GameThreadDispatcher& Instance();

    [[nodiscard]] std::expected<void, std::string> RegisterHook(HookEngine& hooks);
    [[nodiscard]] std::expected<void, std::string> FinalizeResolution(
        HookEngine& hooks);
    [[nodiscard]] std::expected<void, std::string> FinalizeInstallation(
        HookEngine& hooks);

    void SetTickHandler(TickHandler handler);
    void MarkUnavailable(std::string reason);
    void BeginShutdown();
    [[nodiscard]] std::expected<void, std::string> ShutdownHook(
        HookEngine& hooks);
    void Stop();

    [[nodiscard]] GameThreadDispatcherState State() const noexcept {
        return m_state.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool IsReady() const noexcept {
        return State() == GameThreadDispatcherState::Ready;
    }
    [[nodiscard]] bool IsCurrentGameThread() const noexcept;
    [[nodiscard]] std::string UnavailableReason() const;

private:
    using EngineTickFn = void(__fastcall*)(void* engineLoop);

    GameThreadDispatcher() = default;
    ~GameThreadDispatcher() = default;

    GameThreadDispatcher(const GameThreadDispatcher&) = delete;
    GameThreadDispatcher& operator=(const GameThreadDispatcher&) = delete;

    static void __fastcall TickDetour(void* engineLoop) noexcept;
    void DispatchAfterOriginalTick() noexcept;

    std::atomic<GameThreadDispatcherState> m_state{
        GameThreadDispatcherState::Stopped};
    std::atomic<EngineTickFn> m_originalTick{nullptr};
    DetourGate m_detourGate;
    std::atomic<uint32_t> m_activeDetours{0};
    std::atomic<uint32_t> m_activeCallbacks{0};
    std::atomic<uint32_t> m_gameThreadId{0};

    mutable std::mutex m_mutex;
    TickHandler m_tickHandler;
    std::string m_unavailableReason;

#if defined(JST_UNIT_TESTS)
    friend class GameThreadDispatcherTestAccess;
#endif
};

} // namespace jst::core
