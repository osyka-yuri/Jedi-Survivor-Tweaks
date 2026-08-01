#pragma once

#include "detour_gate.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>

namespace jst::core {

class HookEngine;

enum class GameSettingsBarrierState : uint8_t {
    Stopped,
    Resolving,
    WaitingForPostStartupSetter,
    Ready,
    Unavailable,
    Stopping,
};

/**
 * Opens CVar writes only after the game performs its post-startup t.MaxFPS
 * SetByScalability initialization. The setter itself remains untouched;
 * observing its return establishes a second lifecycle barrier after the first
 * post-Tick.
 */
class GameSettingsBarrier final {
public:
    [[nodiscard]] static GameSettingsBarrier& Instance();

    [[nodiscard]] std::expected<void, std::string> RegisterHooks(
        HookEngine& hooks);
    [[nodiscard]] std::expected<void, std::string> FinalizeResolution(
        HookEngine& hooks);
    [[nodiscard]] std::expected<void, std::string> FinalizeInstallation(
        HookEngine& hooks);

    void MarkUnavailable(std::string reason);
    [[nodiscard]] std::expected<void, std::string> ShutdownHooks(
        HookEngine& hooks);

    [[nodiscard]] GameSettingsBarrierState State() const noexcept {
        return m_state.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool IsReady() const noexcept {
        return State() == GameSettingsBarrierState::Ready;
    }
    [[nodiscard]] std::string UnavailableReason() const;

private:
    using StringSetterFn =
        void(__fastcall*)(uintptr_t object, const wchar_t* value, uint32_t setBy);
    using FloatSetterFn =
        void(__fastcall*)(uintptr_t object, float value, uint32_t setBy);
    using IntSetterFn =
        void(__fastcall*)(uintptr_t object, int32_t value, uint32_t setBy);

    GameSettingsBarrier() = default;
    ~GameSettingsBarrier() = default;

    GameSettingsBarrier(const GameSettingsBarrier&) = delete;
    GameSettingsBarrier& operator=(const GameSettingsBarrier&) = delete;

    static void __fastcall StringSetterDetour(
        uintptr_t object,
        const wchar_t* value,
        uint32_t setBy) noexcept;
    static void __fastcall FloatSetterDetour(
        uintptr_t object,
        float value,
        uint32_t setBy) noexcept;
    static void __fastcall IntSetterDetour(
        uintptr_t object,
        int32_t value,
        uint32_t setBy) noexcept;

    void ObserveSetterCompletion(bool onGameThread, uint32_t setBy) noexcept;
    std::atomic<GameSettingsBarrierState> m_state{
        GameSettingsBarrierState::Stopped};
    std::atomic<uintptr_t> m_maxFPSObject{0};
    std::atomic<StringSetterFn> m_originalStringSetter{nullptr};
    std::atomic<FloatSetterFn> m_originalFloatSetter{nullptr};
    std::atomic<IntSetterFn> m_originalIntSetter{nullptr};
    DetourGate m_detourGate;
    std::atomic<uint32_t> m_activeDetours{0};

    mutable std::mutex m_mutex;
    std::string m_unavailableReason;

#if defined(JST_UNIT_TESTS)
    friend class GameSettingsBarrierTestAccess;
#endif
};

} // namespace jst::core
