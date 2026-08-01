#pragma once

#if !defined(JST_UNIT_TESTS)
#error "game_settings_barrier_test_access.hpp is test-only"
#endif

#include "core/game_settings_barrier.hpp"

namespace jst::core {

class GameSettingsBarrierTestAccess final {
public:
    using StringSetter = GameSettingsBarrier::StringSetterFn;
    using FloatSetter = GameSettingsBarrier::FloatSetterFn;
    using IntSetter = GameSettingsBarrier::IntSetterFn;

    static void Reset(
        GameSettingsBarrier& barrier,
        GameSettingsBarrierState state =
            GameSettingsBarrierState::WaitingForPostStartupSetter) {
        barrier.m_maxFPSObject.store(0, std::memory_order_release);
        barrier.m_originalStringSetter.store(nullptr, std::memory_order_release);
        barrier.m_originalFloatSetter.store(nullptr, std::memory_order_release);
        barrier.m_originalIntSetter.store(nullptr, std::memory_order_release);
        barrier.m_activeDetours.store(0, std::memory_order_release);
        barrier.m_state.store(state, std::memory_order_release);
        std::lock_guard lock(barrier.m_mutex);
        barrier.m_unavailableReason.clear();
    }

    static void BindStringSetter(
        GameSettingsBarrier& barrier,
        uintptr_t object,
        StringSetter original) {
        barrier.m_maxFPSObject.store(object, std::memory_order_release);
        barrier.m_originalStringSetter.store(original, std::memory_order_release);
    }

    static void BindFloatSetter(
        GameSettingsBarrier& barrier,
        uintptr_t object,
        FloatSetter original) {
        barrier.m_maxFPSObject.store(object, std::memory_order_release);
        barrier.m_originalFloatSetter.store(original, std::memory_order_release);
    }

    static void BindIntSetter(
        GameSettingsBarrier& barrier,
        uintptr_t object,
        IntSetter original) {
        barrier.m_maxFPSObject.store(object, std::memory_order_release);
        barrier.m_originalIntSetter.store(original, std::memory_order_release);
    }

    static void InvokeStringSetter(
        uintptr_t object,
        const wchar_t* value,
        uint32_t setBy) {
        GameSettingsBarrier::StringSetterDetour(object, value, setBy);
    }

    static void InvokeFloatSetter(
        uintptr_t object,
        float value,
        uint32_t setBy) {
        GameSettingsBarrier::FloatSetterDetour(object, value, setBy);
    }

    static void InvokeIntSetter(
        uintptr_t object,
        int32_t value,
        uint32_t setBy) {
        GameSettingsBarrier::IntSetterDetour(object, value, setBy);
    }

    static void Stop(GameSettingsBarrier& barrier) {
        barrier.m_maxFPSObject.store(0, std::memory_order_release);
        barrier.m_state.store(
            GameSettingsBarrierState::Stopped,
            std::memory_order_release);
    }
};

} // namespace jst::core
