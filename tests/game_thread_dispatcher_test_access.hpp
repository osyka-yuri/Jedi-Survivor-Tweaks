#pragma once

#if !defined(JST_UNIT_TESTS)
#error "game_thread_dispatcher_test_access.hpp is test-only"
#endif

#include "core/game_thread_dispatcher.hpp"

namespace jst::core {

class GameThreadDispatcherTestAccess final {
public:
    using Original = void(__fastcall*)(void*);

    static void Reset(
        GameThreadDispatcher& dispatcher,
        Original original,
        GameThreadDispatcherState state =
            GameThreadDispatcherState::WaitingForFirstTick) {
        dispatcher.Stop();
        dispatcher.m_originalTick.store(original, std::memory_order_release);
        dispatcher.m_activeDetours.store(0, std::memory_order_release);
        dispatcher.m_activeCallbacks.store(0, std::memory_order_release);
        dispatcher.m_gameThreadId.store(0, std::memory_order_release);
        dispatcher.m_state.store(state, std::memory_order_release);
    }

    static void Invoke(GameThreadDispatcher& dispatcher, void* context = nullptr) {
        (void)dispatcher;
        GameThreadDispatcher::TickDetour(context);
    }
};

} // namespace jst::core
