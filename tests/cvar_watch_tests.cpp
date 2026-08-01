#include "core/cvar_system.hpp"
#include "core/cvar_watch_registry.hpp"
#include "cvar_system_test_access.hpp"
#include "cvar_test_fakes.hpp"
#include "cvar_watch_subscription_test_access.hpp"
#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

using jst::core::CVarSystem;
using jst::core::CVarSystemTestAccess;
using jst::core::CVarWatchDecision;
using jst::core::IntWatchRequest;
using cvar_test::Bind;
using cvar_test::FakeCVar;
using cvar_test::FakeVTable;

CVarSystem& Cvars() {
    return CVarSystem::Instance();
}

IntWatchRequest Request(
    std::wstring name,
    std::function<CVarWatchDecision(int32_t)> callback) {
    return IntWatchRequest{
        .name = std::move(name),
        .onValue = std::move(callback),
        .timeout = std::chrono::seconds(5),
    };
}

bool Inject(
    std::wstring_view name,
    FakeCVar& object,
    int32_t& value) {
    return CVarSystemTestAccess::InjectResolved(
        Cvars(),
        name,
        reinterpret_cast<uintptr_t>(&object),
        reinterpret_cast<uintptr_t>(&cvar_test::FakeStringSetter),
        reinterpret_cast<uintptr_t>(&value));
}

} // namespace

void TestCVarWatch() {
    // Continue remains registered; Complete is removed. Callback execution is
    // on the caller of the post-Tick pass, never the resolver thread.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = -1;
        Check(Inject(L"test.Decision", object, value),
              "watch fake injection succeeds");

        int calls = 0;
        std::thread::id callbackThread;
        auto subscription = Cvars().WatchInt(Request(
            L"TEST.decision",
            [&](int32_t observed) {
                ++calls;
                callbackThread = std::this_thread::get_id();
                return observed > 0
                    ? CVarWatchDecision::Complete
                    : CVarWatchDecision::Continue;
            }));
        Check(static_cast<bool>(subscription),
              "valid watch returns move-only ownership");

        const auto gameThread = std::this_thread::get_id();
        CVarSystemTestAccess::PumpOnce(Cvars());
        value = 42;
        CVarSystemTestAccess::PumpOnce(Cvars());
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(calls == 2,
              "Continue observes again and Complete removes the watch");
        Check(callbackThread == gameThread,
              "watch callback runs on the post-Tick game thread");
        Check(CVarSystemTestAccess::WatchEntryCount(Cvars()) == 0,
              "completed watch is removed from the non-owning registry index");
    }

    {
        CVarSystemTestAccess::Reset(Cvars());
        Check(!Cvars().WatchInt({}), "empty watch request is rejected");
        IntWatchRequest missingCallback;
        missingCallback.name = L"test.Empty";
        Check(!Cvars().WatchInt(std::move(missingCallback)),
              "watch without value callback is rejected");
        auto negativeTimeout = Request(
            L"test.NegativeTimeout",
            [](int32_t) { return CVarWatchDecision::Complete; });
        negativeTimeout.timeout = std::chrono::milliseconds(-1);
        Check(!Cvars().WatchInt(std::move(negativeTimeout)),
              "watch with a negative timeout is rejected");
    }

    // Startup time before the first completed engine Tick does not consume a
    // watch timeout budget. The barrier rebases all pre-Tick deadlines.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = 9;
        Check(Inject(L"test.StartupDeadline", object, value),
              "startup deadline fake injection succeeds");
        int valueCalls = 0;
        int timeouts = 0;
        auto request = Request(L"test.StartupDeadline", [&](int32_t) {
            ++valueCalls;
            return CVarWatchDecision::Complete;
        });
        request.timeout = std::chrono::milliseconds(10);
        request.onTimeout = [&] { ++timeouts; };
        auto subscription = Cvars().WatchInt(std::move(request));
        std::mutex delayMutex;
        std::condition_variable delayCv;
        std::unique_lock delayLock(delayMutex);
        (void)delayCv.wait_for(delayLock, std::chrono::milliseconds(20));
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(valueCalls == 1 && timeouts == 0,
              "pre-barrier startup time does not expire a watch");
    }

    // Abort has priority over timeout, and timeout has priority over reading.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = 7;
        Check(Inject(L"test.Priority", object, value),
              "priority fake injection succeeds");
        int valueCalls = 0;
        int timeouts = 0;

        auto abortedRequest = Request(L"test.Priority", [&](int32_t) {
            ++valueCalls;
            return CVarWatchDecision::Complete;
        });
        abortedRequest.timeout = std::chrono::milliseconds(0);
        abortedRequest.shouldAbort = [] { return true; };
        abortedRequest.onTimeout = [&] { ++timeouts; };
        auto aborted = Cvars().WatchInt(std::move(abortedRequest));
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(valueCalls == 0 && timeouts == 0,
              "abort completes silently before timeout and value");

        auto timedRequest = Request(L"test.Priority", [&](int32_t) {
            ++valueCalls;
            return CVarWatchDecision::Complete;
        });
        timedRequest.timeout = std::chrono::milliseconds(0);
        timedRequest.onTimeout = [&] { ++timeouts; };
        auto timed = Cvars().WatchInt(std::move(timedRequest));
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(valueCalls == 0 && timeouts == 1,
              "timeout completes before any value callback");
    }

    // User callbacks execute without registry/cache mutexes. They can enqueue
    // a CVar and register another watch; both become visible next Tick.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable firstTable;
        FakeVTable secondTable;
        FakeCVar firstObject;
        FakeCVar secondObject;
        Bind(firstObject, firstTable);
        Bind(secondObject, secondTable);
        int32_t firstValue = 1;
        int32_t secondValue = 0;
        secondObject.setterTarget = &secondValue;
        Check(Inject(L"test.First", firstObject, firstValue) &&
                  Inject(L"test.Second", secondObject, secondValue),
              "reentrant watch fakes inject");

        int nestedCalls = 0;
        jst::core::CVarWatchSubscription nested;
        auto first = Cvars().WatchInt(Request(L"test.First", [&](int32_t) {
            (void)Cvars().SetInt(L"test.Second", 8);
            nested = Cvars().WatchInt(Request(
                L"test.Second",
                [&](int32_t observed) {
                    ++nestedCalls;
                    return observed == 8
                        ? CVarWatchDecision::Complete
                        : CVarWatchDecision::Continue;
                }));
            return CVarWatchDecision::Complete;
        }));

        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(secondValue == 0 && nestedCalls == 0,
              "work enqueued by a callback waits for the next Tick snapshot");
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(secondValue == 8 && nestedCalls == 1,
              "reentrant Set and Watch complete on the following Tick");
    }

    // One throwing callback is cancelled without preventing later watches.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = 5;
        Check(Inject(L"test.Exceptions", object, value),
              "exception fake injection succeeds");
        int survivorCalls = 0;
        auto throwing = Cvars().WatchInt(Request(
            L"test.Exceptions",
            [](int32_t) -> CVarWatchDecision {
                throw std::runtime_error("expected");
            }));
        auto survivor = Cvars().WatchInt(Request(
            L"TEST.exceptions",
            [&](int32_t) {
                ++survivorCalls;
                return CVarWatchDecision::Complete;
            }));
        CVarSystemTestAccess::PumpOnce(Cvars());
        Check(survivorCalls == 1,
              "callback exception is isolated from later watches");
    }

    // Reset remains a join barrier after a concurrent Clear has already
    // detached the registry entry.
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;
        bool release = false;
        jst::core::CVarWatchRegistry registry;
        auto control = registry.Register(Request(
            L"test.CancelBarrier",
            [&](int32_t) {
                std::unique_lock lock(mutex);
                entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
                return CVarWatchDecision::Complete;
            }));
        auto subscription =
            jst::core::CVarWatchSubscriptionTestAccess::Make(control);

        std::thread evaluator([&] {
            registry.Evaluate([](std::wstring_view) {
                return std::optional<int32_t>{1};
            });
        });
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return entered; });
        }

        std::promise<void> clearReturned;
        auto clearReturnedFuture = clearReturned.get_future();
        std::thread clearer([&] {
            registry.Clear();
            clearReturned.set_value();
        });
        Check(registry.WaitUntilAbsent(
                  L"test.CancelBarrier",
                  std::chrono::seconds(2)),
              "Clear detaches the active watch before subscription reset");

        std::promise<void> resetStarted;
        std::promise<void> resetReturned;
        auto resetStartedFuture = resetStarted.get_future();
        auto resetReturnedFuture = resetReturned.get_future();
        std::thread resetter([&] {
            resetStarted.set_value();
            subscription.Reset();
            resetReturned.set_value();
        });
        Check(resetStartedFuture.wait_for(std::chrono::seconds(2)) ==
                  std::future_status::ready &&
                  resetReturnedFuture.wait_for(std::chrono::milliseconds(20)) ==
                      std::future_status::timeout,
              "subscription Reset waits for its active callback");
        Check(clearReturnedFuture.wait_for(std::chrono::milliseconds(20)) ==
                  std::future_status::timeout,
              "Clear waits for the detached active watch");
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
        evaluator.join();
        resetter.join();
        clearer.join();
        Check(resetReturnedFuture.wait_for(std::chrono::seconds(2)) ==
                  std::future_status::ready &&
                  clearReturnedFuture.wait_for(std::chrono::seconds(2)) ==
                      std::future_status::ready,
              "both cancellation barriers return after callback completion");
    }

    // The inverse ordering has the same join semantics: subscription Reset
    // closes its control first, then a concurrent registry Clear retains that
    // detached/inactive control until the callback exits.
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;
        bool release = false;
        jst::core::CVarWatchRegistry registry;
        auto control = registry.Register(Request(
            L"test.ResetFirstBarrier",
            [&](int32_t) {
                std::unique_lock lock(mutex);
                entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
                return CVarWatchDecision::Complete;
            }));
        auto subscription =
            jst::core::CVarWatchSubscriptionTestAccess::Make(control);

        std::thread evaluator([&] {
            registry.Evaluate([](std::wstring_view) {
                return std::optional<int32_t>{1};
            });
        });
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return entered; });
        }

        std::promise<void> resetStarted;
        std::promise<void> resetReturned;
        auto resetStartedFuture = resetStarted.get_future();
        auto resetReturnedFuture = resetReturned.get_future();
        std::thread resetter([&] {
            resetStarted.set_value();
            subscription.Reset();
            resetReturned.set_value();
        });
        Check(resetStartedFuture.wait_for(std::chrono::seconds(2)) ==
                  std::future_status::ready &&
                  registry.WaitUntilAbsent(
                      L"test.ResetFirstBarrier",
                      std::chrono::seconds(2)) &&
                  resetReturnedFuture.wait_for(std::chrono::milliseconds(20)) ==
                      std::future_status::timeout,
              "Reset-first cancellation closes admission but remains a join barrier");

        std::promise<void> clearStarted;
        std::promise<void> clearReturned;
        auto clearStartedFuture = clearStarted.get_future();
        auto clearReturnedFuture = clearReturned.get_future();
        std::thread clearer([&] {
            clearStarted.set_value();
            registry.Clear();
            clearReturned.set_value();
        });
        Check(clearStartedFuture.wait_for(std::chrono::seconds(2)) ==
                  std::future_status::ready &&
                  clearReturnedFuture.wait_for(std::chrono::milliseconds(20)) ==
                      std::future_status::timeout,
              "Clear also waits when Reset won the active-callback race");

        {
            std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
        evaluator.join();
        resetter.join();
        clearer.join();
        Check(resetReturnedFuture.wait_for(std::chrono::seconds(2)) ==
                  std::future_status::ready &&
                  clearReturnedFuture.wait_for(std::chrono::seconds(2)) ==
                      std::future_status::ready &&
                  registry.EntryCount() == 0,
              "Reset-first and Clear cancellation both finish after callback exit");
    }

    // System shutdown has the same barrier and rejects later observations.
    {
        CVarSystemTestAccess::Reset(Cvars());
        FakeVTable vtable;
        FakeCVar object;
        Bind(object, vtable);
        int32_t value = 1;
        Check(Inject(L"test.StopBarrier", object, value),
              "stop barrier fake injection succeeds");

        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;
        bool release = false;
        auto subscription = Cvars().WatchInt(Request(
            L"test.StopBarrier",
            [&](int32_t) {
                std::unique_lock lock(mutex);
                entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
                return CVarWatchDecision::Complete;
            }));
        std::thread gameThread([] {
            CVarSystemTestAccess::PumpOnce(Cvars());
        });
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return entered; });
        }

        std::promise<void> stopStarted;
        std::promise<void> stopReturned;
        auto stopStartedFuture = stopStarted.get_future();
        auto stopReturnedFuture = stopReturned.get_future();
        std::thread stopper([&] {
            stopStarted.set_value();
            Cvars().Stop();
            stopReturned.set_value();
        });
        Check(stopStartedFuture.wait_for(std::chrono::seconds(2)) ==
                  std::future_status::ready &&
                  stopReturnedFuture.wait_for(std::chrono::milliseconds(20)) ==
                      std::future_status::timeout,
              "CVar shutdown waits for an in-flight callback");
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
        gameThread.join();
        stopper.join();
        Check(stopReturnedFuture.wait_for(std::chrono::seconds(2)) ==
                  std::future_status::ready &&
                  !Cvars().WatchInt(Request(
                      L"test.AfterStop",
                      [](int32_t) { return CVarWatchDecision::Complete; })) &&
                  CVarSystemTestAccess::WatchEntryCount(Cvars()) == 0,
              "shutdown clears registry and rejects new watches");
    }
}
