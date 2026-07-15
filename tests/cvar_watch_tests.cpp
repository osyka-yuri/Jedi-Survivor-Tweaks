#include "core/cvar_system.hpp"
#include "core/cvar_layout.hpp"
#include "core/cvar_resolver.hpp"
#include "cvar_system_test_access.hpp"
#include "test_check.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

using jst::core::CVarSystem;
using jst::core::CVarSystemTestAccess;
using jst::core::CVarWatchDecision;
using jst::core::CVarWatchSubscription;
using jst::core::IntWatchRequest;

namespace {

CVarSystem& Cvs() {
    return CVarSystem::Instance();
}

IntWatchRequest Request(
    std::wstring name,
    std::function<CVarWatchDecision(int32_t)> onValue) {
    IntWatchRequest request;
    request.name = std::move(name);
    request.onValue = std::move(onValue);
    request.timeout = std::chrono::seconds{5};
    return request;
}

} // namespace

void TestCVarWatch() {
    // A CVar first requested after startup already carries a SetBy priority in
    // its flags. It must resolve just like a constructor-priority object.
    {
        alignas(uintptr_t) std::array<uint8_t, 80> object{};
        alignas(uintptr_t) uintptr_t vtableStorage = 0;
        alignas(uintptr_t) uintptr_t globalPointer =
            reinterpret_cast<uintptr_t>(object.data());
        *reinterpret_cast<uintptr_t*>(object.data()) =
            reinterpret_cast<uintptr_t>(&vtableStorage);
        *reinterpret_cast<uint32_t*>(
            object.data() + jst::core::cvar_layout::kFlagsOffset) = 0x03000040;
        *reinterpret_cast<int32_t*>(
            object.data() + jst::core::cvar_layout::kValueOffset) = 4000;

        jst::core::ScanEntry scan;
        scan.globalPtrCandidates.push_back(
            reinterpret_cast<uintptr_t>(&globalPointer));
        const auto resolved = jst::core::ResolveFromScan(
            scan, jst::core::ModuleInfo{}, nullptr);
        Check(resolved && resolved->writeAddr ==
                  reinterpret_cast<uintptr_t>(object.data()) +
                      jst::core::cvar_layout::kValueOffset,
              "late CVar resolution accepts project-setting priority flags");

        *reinterpret_cast<uint32_t*>(
            object.data() + jst::core::cvar_layout::kFlagsOffset) = 0x0B000000;
        Check(!jst::core::ResolveFromScan(scan, jst::core::ModuleInfo{}, nullptr),
              "CVar resolution rejects an unknown priority above console");
    }

    // Direct primitive candidates must reject UTF-16 text fragments while
    // retaining ordinary integer CVars.
    {
        alignas(IMAGE_NT_HEADERS) std::array<uint8_t, 0x500> image{};
        const uintptr_t base = reinterpret_cast<uintptr_t>(image.data());
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.NumberOfSections = 1;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader);
        auto* data = IMAGE_FIRST_SECTION(nt);
        data->Name[0] = '.';
        data->Name[1] = 'd';
        data->Name[2] = 'a';
        data->Name[3] = 't';
        data->Name[4] = 'a';
        data->VirtualAddress = 0x400;
        data->Misc.VirtualSize = 0x100;

        auto* storage = reinterpret_cast<uintptr_t*>(image.data() + 0x420);
        *storage = 0x0072006F; // UTF-16 "or", previously accepted as a subnormal float.
        jst::core::ScanEntry scan;
        scan.refVarCandidates.push_back(reinterpret_cast<uintptr_t>(storage));
        const jst::core::ModuleInfo module{base, image.size()};
        Check(!jst::core::ResolveFromScan(scan, module, nullptr),
              "UTF-16 text is not accepted as direct CVar storage");

        *storage = 4000;
        const auto resolved = jst::core::ResolveFromScan(scan, module, nullptr);
        Check(resolved && resolved->writeAddr == reinterpret_cast<uintptr_t>(storage),
              "ordinary integer direct CVar storage remains valid");
    }

    // Continue observes repeatedly; Complete removes the registry entry.
    {
        CVarSystemTestAccess::Reset(Cvs());
        int32_t storage = -1;
        int calls = 0;
        Check(CVarSystemTestAccess::InjectResolvedInt(Cvs(), L"test.Decision", &storage),
              "inject resolved CVar for decisions");

        auto subscription = Cvs().WatchInt(Request(L"test.Decision", [&](int32_t value) {
            ++calls;
            return value > 0 ? CVarWatchDecision::Complete
                             : CVarWatchDecision::Continue;
        }));
        Check(static_cast<bool>(subscription), "valid request returns a subscription");
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(calls == 1, "Continue keeps the watch active");
        storage = 42;
        CVarSystemTestAccess::PumpOnce(Cvs());
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(calls == 2, "Complete removes the watch after the winning value");
    }

    // Empty requests are rejected without creating ownership.
    {
        CVarSystemTestAccess::Reset(Cvs());
        Check(!Cvs().WatchInt({}), "empty watch request is rejected");
        IntWatchRequest missingCallback;
        missingCallback.name = L"test.Empty";
        Check(!Cvs().WatchInt(std::move(missingCallback)),
              "watch without onValue is rejected");
    }

    // Priority is abort -> timeout -> value.
    {
        CVarSystemTestAccess::Reset(Cvs());
        int32_t storage = 7;
        Check(CVarSystemTestAccess::InjectResolvedInt(Cvs(), L"test.Priority", &storage),
              "inject resolved CVar for priority");
        int values = 0;
        int timeouts = 0;

        auto abortedRequest = Request(L"test.Priority", [&](int32_t) {
            ++values;
            return CVarWatchDecision::Complete;
        });
        abortedRequest.timeout = std::chrono::milliseconds{0};
        abortedRequest.shouldAbort = [] { return true; };
        abortedRequest.onTimeout = [&] { ++timeouts; };
        auto aborted = Cvs().WatchInt(std::move(abortedRequest));
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(values == 0 && timeouts == 0, "abort completes silently before timeout/value");

        auto timedRequest = Request(L"test.Priority", [&](int32_t) {
            ++values;
            return CVarWatchDecision::Complete;
        });
        timedRequest.timeout = std::chrono::milliseconds{0};
        timedRequest.onTimeout = [&] { ++timeouts; };
        auto timed = Cvs().WatchInt(std::move(timedRequest));
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(timeouts == 1 && values == 0, "timeout wins over a ready value at deadline");
    }

    // Reset and destruction cancel before a callback can begin.
    {
        CVarSystemTestAccess::Reset(Cvs());
        int32_t storage = 1;
        int calls = 0;
        Check(CVarSystemTestAccess::InjectResolvedInt(Cvs(), L"test.Cancel", &storage),
              "inject resolved CVar for cancellation");
        auto subscription = Cvs().WatchInt(Request(L"test.Cancel", [&](int32_t) {
            ++calls;
            return CVarWatchDecision::Complete;
        }));
        subscription.Reset();
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(calls == 0, "Reset prevents a future callback");

        {
            auto scoped = Cvs().WatchInt(Request(L"test.Cancel", [&](int32_t) {
                ++calls;
                return CVarWatchDecision::Complete;
            }));
        }
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(calls == 0, "destruction prevents a future callback");
    }

    // Reset is a join barrier for an onValue already in flight.
    {
        CVarSystemTestAccess::Reset(Cvs());
        int32_t storage = 9;
        Check(CVarSystemTestAccess::InjectResolvedInt(Cvs(), L"test.Join", &storage),
              "inject resolved CVar for reset barrier");
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;
        bool release = false;
        std::atomic<bool> callbackFinished{false};
        std::atomic<bool> resetReturned{false};

        auto subscription = Cvs().WatchInt(Request(L"test.Join", [&](int32_t) {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
            callbackFinished.store(true, std::memory_order_release);
            return CVarWatchDecision::Complete;
        }));
        Cvs().StartPump(std::chrono::milliseconds{1});
        {
            std::unique_lock lock(mutex);
            Check(cv.wait_for(lock, std::chrono::seconds{2}, [&] { return entered; }),
                  "onValue entered before barrier test");
        }
        std::thread resetter([&] {
            subscription.Reset();
            resetReturned.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        Check(!resetReturned.load(std::memory_order_acquire),
              "Reset waits for an in-flight onValue");
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
        resetter.join();
        Check(callbackFinished.load(std::memory_order_acquire) &&
                  resetReturned.load(std::memory_order_acquire),
              "Reset returns only after onValue finishes");
        Cvs().StopPump();
    }

    // Move-assignment joins/cancels the prior ownership and transfers the new one.
    {
        CVarSystemTestAccess::Reset(Cvs());
        int32_t firstValue = 1;
        int32_t secondValue = 2;
        int firstCalls = 0;
        int secondCalls = 0;
        Check(CVarSystemTestAccess::InjectResolvedInt(Cvs(), L"test.MoveFirst", &firstValue),
              "inject first move CVar");
        Check(CVarSystemTestAccess::InjectResolvedInt(Cvs(), L"test.MoveSecond", &secondValue),
              "inject second move CVar");
        auto first = Cvs().WatchInt(Request(L"test.MoveFirst", [&](int32_t) {
            ++firstCalls;
            return CVarWatchDecision::Complete;
        }));
        auto second = Cvs().WatchInt(Request(L"test.MoveSecond", [&](int32_t) {
            ++secondCalls;
            return CVarWatchDecision::Complete;
        }));
        first = std::move(second);
        Check(!second, "move assignment empties the source subscription");
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(firstCalls == 0 && secondCalls == 1,
              "move assignment cancels old watch and retains new watch");
    }

    // Callback failures are isolated; a callback may safely use resolver APIs.
    {
        CVarSystemTestAccess::Reset(Cvs());
        int32_t storage = 7;
        int healthyCalls = 0;
        Check(CVarSystemTestAccess::InjectResolvedInt(
                  Cvs(), L"test.ExceptionIsolation", &storage),
              "inject resolved CVar for exception isolation");
        auto throwing = Cvs().WatchInt(Request(L"test.ExceptionIsolation", [](int32_t) -> CVarWatchDecision {
            throw std::runtime_error("expected");
        }));
        auto healthy = Cvs().WatchInt(Request(L"test.ExceptionIsolation", [&](int32_t) {
            ++healthyCalls;
            Check(Cvs().SetInt(L"test.ExceptionIsolation", 8),
                  "onValue may reenter resolved SetInt");
            return CVarWatchDecision::Complete;
        }));
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(healthyCalls == 1 && storage == 8,
              "throwing watch does not stop a reentrant healthy watch");
    }

    // A callback may cancel a later subscription before that callback starts.
    {
        CVarSystemTestAccess::Reset(Cvs());
        int32_t storage = 1;
        Check(CVarSystemTestAccess::InjectResolvedInt(Cvs(), L"test.ReentrantCancel", &storage),
              "inject resolved CVar for reentrant cancellation");
        int firstCalls = 0;
        int cancelledCalls = 0;
        CVarWatchSubscription later;
        auto first = Cvs().WatchInt(Request(L"test.ReentrantCancel", [&](int32_t) {
            ++firstCalls;
            later.Reset();
            return CVarWatchDecision::Complete;
        }));
        later = Cvs().WatchInt(Request(L"test.ReentrantCancel", [&](int32_t) {
            ++cancelledCalls;
            return CVarWatchDecision::Complete;
        }));
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(firstCalls == 1 && cancelledCalls == 0,
              "reentrant cancellation suppresses a later callback without deadlock");
    }

    // StopPump is also a barrier and clears outstanding watches.
    {
        CVarSystemTestAccess::Reset(Cvs());
        int32_t storage = 3;
        Check(CVarSystemTestAccess::InjectResolvedInt(Cvs(), L"test.StopPump", &storage),
              "inject resolved CVar for StopPump barrier");
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;
        bool release = false;
        std::atomic<bool> stopReturned{false};
        auto subscription = Cvs().WatchInt(Request(L"test.StopPump", [&](int32_t) {
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
            return CVarWatchDecision::Continue;
        }));
        Cvs().StartPump(std::chrono::milliseconds{1});
        {
            std::unique_lock lock(mutex);
            Check(cv.wait_for(lock, std::chrono::seconds{2}, [&] { return entered; }),
                  "callback entered before StopPump barrier");
        }
        std::thread stopper([&] {
            Cvs().StopPump();
            stopReturned.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        Check(!stopReturned.load(std::memory_order_acquire),
              "StopPump waits for an in-flight callback");
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
        stopper.join();
        Check(stopReturned.load(std::memory_order_acquire),
              "StopPump returns after callback and registry shutdown");
        subscription.Reset();
    }

    // Deferred writes retain only their latest target.
    {
        CVarSystemTestAccess::Reset(Cvs());
        int32_t storage = 0;
        constexpr auto name = L"test.LatestPendingTarget";
        Check(!Cvs().SetInt(name, 1), "first unresolved write is deferred");
        Check(!Cvs().SetInt(name, 2), "second unresolved write replaces target");
        Check(CVarSystemTestAccess::CommitPendingAsResolvedInt(Cvs(), name, &storage),
              "pending CVar commits as resolved");
        Check(storage == 2, "resolution writes the latest deferred target");
        Check(Cvs().SetInt(name, 3) && storage == 3,
              "resolved writes remain synchronous");
    }

    // Resolution is retained when an initial write only reaches the primary
    // address. The pump retries the target without rescanning the CVar.
    {
        CVarSystemTestAccess::Reset(Cvs());
        int32_t primary = 0;
        int32_t shadow = 0;
        constexpr auto name = L"test.RetryResolvedWrite";
        Check(!Cvs().SetInt(name, 17), "unresolved write is deferred before half-write test");
        Check(CVarSystemTestAccess::CommitPendingAsResolvedInt(Cvs(), name, &primary, 1),
              "CVar remains resolved when the initial shadow address rejects the write");
        Check(primary == 17 && CVarSystemTestAccess::HasPendingWrite(Cvs(), name),
              "primary write is visible while the complete target remains pending");
        Check(CVarSystemTestAccess::SetResolvedShadowAddress(
                  Cvs(), name, reinterpret_cast<uintptr_t>(&shadow)),
              "test repairs the transient shadow address without another resolve");
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(primary == 17 && shadow == 17 &&
                  !CVarSystemTestAccess::HasPendingWrite(Cvs(), name),
              "pump completes the retained resolved write and clears pending state");
    }

    {
        CVarSystemTestAccess::Reset(Cvs());
        CVarSystemTestAccess::SetPendingTimeout(Cvs(), std::chrono::milliseconds{1});
        int32_t primary = 0;
        constexpr auto name = L"test.ResolvedWriteTimeout";
        Check(!Cvs().SetInt(name, 23), "write-timeout target starts unresolved");
        Check(CVarSystemTestAccess::CommitPendingAsResolvedInt(Cvs(), name, &primary, 1),
              "write-timeout target commits its resolved address");
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(CVarSystemTestAccess::HasCacheEntry(Cvs(), name) &&
                  !CVarSystemTestAccess::HasPendingWrite(Cvs(), name),
              "write timeout clears only the target and retains resolved cache");
    }

    // A temporarily unavailable game module requeues, rather than loses, scan work.
    {
        CVarSystemTestAccess::Reset(Cvs());
        CVarSystemTestAccess::SimulateModuleUnavailable(Cvs());
        Check(!Cvs().SetInt(L"test.RetryModule", 1),
              "unresolved CVar queues an initial scan");
        Check(CVarSystemTestAccess::InitialScanQueueSize(Cvs()) == 1,
              "initial scan queue contains the unresolved CVar");
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(CVarSystemTestAccess::InitialScanQueueSize(Cvs()) == 1,
              "unavailable module preserves scan work for a later pump");
    }

    // An active subscription keeps an aged pending CVar alive.
    {
        CVarSystemTestAccess::Reset(Cvs());
        CVarSystemTestAccess::SetPendingTimeout(Cvs(), std::chrono::milliseconds{1});
        constexpr auto name = L"test.KeepAlive";
        Check(CVarSystemTestAccess::InjectAgedUnresolvedPending(Cvs(), name, 1),
              "inject aged unresolved pending CVar");
        int calls = 0;
        auto subscription = Cvs().WatchInt(Request(name, [&](int32_t) {
            ++calls;
            return CVarWatchDecision::Complete;
        }));
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(CVarSystemTestAccess::HasCacheEntry(Cvs(), name),
              "active watch keeps aged pending CVar alive");
        int32_t storage = 33;
        Check(CVarSystemTestAccess::InjectResolvedInt(Cvs(), name, &storage),
              "late CVar resolution is injectable");
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(calls == 1, "watch completes after late CVar resolution");
    }

    {
        CVarSystemTestAccess::Reset(Cvs());
        CVarSystemTestAccess::SetPendingTimeout(Cvs(), std::chrono::milliseconds{1});
        constexpr auto name = L"test.DropAged";
        Check(CVarSystemTestAccess::InjectAgedUnresolvedPending(Cvs(), name, 1),
              "inject aged pending CVar without watch");
        CVarSystemTestAccess::PumpOnce(Cvs());
        Check(!CVarSystemTestAccess::HasCacheEntry(Cvs(), name),
              "aged pending CVar is dropped without an observer");
    }

    CVarSystemTestAccess::Reset(Cvs());
}
