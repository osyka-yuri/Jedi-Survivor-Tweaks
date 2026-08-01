#include "core/game_thread_dispatcher.hpp"
#include "core/hook_engine.hpp"
#include "game_thread_dispatcher_test_access.hpp"
#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using jst::core::GameThreadDispatcher;
using jst::core::GameThreadDispatcherState;
using jst::core::GameThreadDispatcherTestAccess;

std::vector<int>* g_order = nullptr;
void __fastcall OrderedOriginal(void*) {
    g_order->push_back(1);
}

std::mutex g_blockMutex;
std::condition_variable g_blockCv;
bool g_blockEntered = false;
bool g_blockRelease = false;
void __fastcall BlockingOriginal(void*) {
    std::unique_lock lock(g_blockMutex);
    g_blockEntered = true;
    g_blockCv.notify_all();
    g_blockCv.wait(lock, [] { return g_blockRelease; });
}

std::mutex g_callbackMutex;
std::condition_variable g_callbackCv;
bool g_callbackEntered = false;
bool g_callbackRelease = false;

} // namespace

void TestGameThreadDispatcher() {
    auto& dispatcher = GameThreadDispatcher::Instance();

    {
        std::vector<int> order;
        g_order = &order;
        GameThreadDispatcherTestAccess::Reset(
            dispatcher, &OrderedOriginal);
        dispatcher.SetTickHandler([&] { order.push_back(2); });

        GameThreadDispatcherTestAccess::Invoke(dispatcher);
        Check(order == std::vector<int>({1, 2}),
              "dispatcher always calls original Tick before queued work");
        Check(dispatcher.State() == GameThreadDispatcherState::Ready,
              "first completed original Tick opens the startup barrier");
        Check(dispatcher.IsCurrentGameThread(),
              "first completed Tick records the current game thread");

        bool workerRecognizedAsGameThread = true;
        std::thread worker([&] {
            workerRecognizedAsGameThread = dispatcher.IsCurrentGameThread();
        });
        worker.join();
        Check(!workerRecognizedAsGameThread,
              "dispatcher rejects a different thread after startup");

        GameThreadDispatcherTestAccess::Invoke(dispatcher);
        Check(order == std::vector<int>({1, 2, 1, 2}),
              "ready dispatcher performs one post-Tick pass per Tick");
    }

    // Quiescing clears callbacks but deliberately retains the trampoline until
    // HookEngine removes the splice.
    {
        std::vector<int> order;
        g_order = &order;
        GameThreadDispatcherTestAccess::Reset(
            dispatcher,
            &OrderedOriginal,
            GameThreadDispatcherState::Ready);
        dispatcher.SetTickHandler([&] { order.push_back(2); });
        dispatcher.BeginShutdown();
        GameThreadDispatcherTestAccess::Invoke(dispatcher);
        Check(order == std::vector<int>({1}),
              "quiesced detour still calls original but never dispatches work");
    }

    // Callback admission closes independently from detour lifetime, and
    // BeginShutdown waits for a callback already admitted before shutdown.
    {
        std::vector<int> order;
        g_order = &order;
        {
            std::lock_guard lock(g_callbackMutex);
            g_callbackEntered = false;
            g_callbackRelease = false;
        }
        GameThreadDispatcherTestAccess::Reset(
            dispatcher,
            &OrderedOriginal,
            GameThreadDispatcherState::Ready);
        dispatcher.SetTickHandler([] {
            std::unique_lock lock(g_callbackMutex);
            g_callbackEntered = true;
            g_callbackCv.notify_all();
            g_callbackCv.wait(lock, [] { return g_callbackRelease; });
        });

        std::thread gameThread([&] {
            GameThreadDispatcherTestAccess::Invoke(dispatcher);
        });
        {
            std::unique_lock lock(g_callbackMutex);
            g_callbackCv.wait(lock, [] { return g_callbackEntered; });
        }

        std::promise<void> shutdownStarted;
        std::promise<void> shutdownReturned;
        auto shutdownStartedFuture = shutdownStarted.get_future();
        auto shutdownFuture = shutdownReturned.get_future();
        std::thread shutdown([&] {
            shutdownStarted.set_value();
            dispatcher.BeginShutdown();
            shutdownReturned.set_value();
        });
        Check(shutdownStartedFuture.wait_for(std::chrono::seconds(2)) ==
                  std::future_status::ready &&
                  shutdownFuture.wait_for(std::chrono::milliseconds(20)) ==
                      std::future_status::timeout,
              "callback admission shutdown waits for an active callback");
        {
            std::lock_guard lock(g_callbackMutex);
            g_callbackRelease = true;
        }
        g_callbackCv.notify_all();
        gameThread.join();
        shutdown.join();
        Check(shutdownFuture.wait_for(std::chrono::seconds(2)) ==
                  std::future_status::ready,
              "callback barrier returns after the admitted callback exits");
    }

    // A detour blocked inside original Tick has not admitted a callback.
    // Admission can close immediately; hook removal then waits for the detour
    // while the retained trampoline lets it finish safely.
    {
        {
            std::lock_guard lock(g_blockMutex);
            g_blockEntered = false;
            g_blockRelease = false;
        }
        GameThreadDispatcherTestAccess::Reset(
            dispatcher,
            &BlockingOriginal,
            GameThreadDispatcherState::Ready);

        std::thread gameThread([&] {
            GameThreadDispatcherTestAccess::Invoke(dispatcher);
        });
        {
            std::unique_lock lock(g_blockMutex);
            g_blockCv.wait(lock, [] { return g_blockEntered; });
        }

        dispatcher.BeginShutdown();

        jst::core::HookEngine hooks;
        std::promise<void> removalStarted;
        std::promise<void> removalReturned;
        auto removalStartedFuture = removalStarted.get_future();
        auto removalFuture = removalReturned.get_future();
        std::thread removal([&] {
            removalStarted.set_value();
            (void)dispatcher.ShutdownHook(hooks);
            removalReturned.set_value();
        });
        Check(removalStartedFuture.wait_for(std::chrono::seconds(2)) ==
                  std::future_status::ready &&
                  removalFuture.wait_for(std::chrono::milliseconds(20)) ==
                      std::future_status::timeout,
              "hook removal waits for a detour still inside original Tick");

        {
            std::lock_guard lock(g_blockMutex);
            g_blockRelease = true;
        }
        g_blockCv.notify_all();
        gameThread.join();
        removal.join();
        Check(removalFuture.wait_for(std::chrono::seconds(2)) ==
                  std::future_status::ready &&
                  dispatcher.State() == GameThreadDispatcherState::Stopped,
              "retained trampoline lets the old detour finish after unhook");
    }

    dispatcher.Stop();
}
