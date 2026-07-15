#include "core/graphics_adapter_service.hpp"
#include "graphics_adapter_service_test_access.hpp"
#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

void TestGraphicsAdapterService() {
    using jst::core::GraphicsAdapterId;
    using jst::core::GraphicsAdapterService;
    using jst::core::GraphicsAdapterServiceTestAccess;
    using jst::core::GraphicsAdapterSnapshot;

    auto& service = GraphicsAdapterService::Instance();
    GraphicsAdapterServiceTestAccess::Reset(service);

    constexpr GraphicsAdapterId gameId{.lowPart = 7, .highPart = 3};
    Check(gameId == GraphicsAdapterId{.lowPart = 7, .highPart = 3},
          "matching adapter IDs compare equal");
    Check(gameId != GraphicsAdapterId{.lowPart = 7, .highPart = 4},
          "different adapter IDs never compare equal");
    Check(GraphicsAdapterServiceTestAccess::Classify(gameId, 24ull << 30, false)
              .HasDedicatedVideoMemory(),
          "hardware adapter publishes dedicated VRAM");
    Check(!GraphicsAdapterServiceTestAccess::Classify(gameId, 0, false)
               .HasDedicatedVideoMemory(),
          "UMA adapter publishes no fixed dedicated VRAM");
    Check(!GraphicsAdapterServiceTestAccess::Classify(gameId, 24ull << 30, true)
               .HasDedicatedVideoMemory(),
          "software adapter publishes no dedicated VRAM");

    Check(!service.Subscribe({}), "empty callback does not create a subscription");

    int callbacks = 0;
    GraphicsAdapterSnapshot last{};
    auto subscription = service.Subscribe([&](const GraphicsAdapterSnapshot& snapshot) {
        ++callbacks;
        last = snapshot;
    });
    Check(callbacks == 1 && last == GraphicsAdapterSnapshot{},
          "subscription immediately receives the current snapshot");

    const GraphicsAdapterSnapshot detected{
        .id = gameId,
        .dedicatedVideoMemoryBytes = 24ull << 30,
    };
    GraphicsAdapterServiceTestAccess::ApplyCandidate(service, detected);
    Check(callbacks == 2 && last == detected,
          "new adapter snapshot reaches the subscriber");
    GraphicsAdapterServiceTestAccess::ApplyCandidate(service, detected);
    Check(callbacks == 2, "identical adapter publication is deduplicated");

    // A transient failure for the same adapter cannot erase confirmed capacity.
    GraphicsAdapterServiceTestAccess::ApplyCandidate(
        service, GraphicsAdapterSnapshot{.id = gameId});
    Check(service.Snapshot() == detected,
          "same-adapter transient failure preserves confirmed VRAM");
    Check(callbacks == 2, "non-regression path does not re-notify subscribers");

    // A changed identity is meaningful even if capacity is not yet known.
    constexpr GraphicsAdapterId otherId{.lowPart = 1, .highPart = 2};
    GraphicsAdapterServiceTestAccess::ApplyCandidate(
        service, GraphicsAdapterSnapshot{.id = otherId});
    Check(service.Snapshot() == GraphicsAdapterSnapshot{.id = otherId},
          "new adapter identity replaces the previous snapshot");
    Check(callbacks == 3, "adapter identity change notifies subscribers once");

    subscription.Reset();
    GraphicsAdapterServiceTestAccess::Publish(service, {});
    Check(callbacks == 3, "unsubscribed callback receives no later publications");

    // Initial delivery and a racing publication are serialized in snapshot order.
    {
        std::mutex orderMutex;
        std::condition_variable orderCv;
        bool initialEntered = false;
        bool releaseInitial = false;
        std::vector<GraphicsAdapterSnapshot> observed;
        GraphicsAdapterService::Subscription ordered;
        std::thread subscriber([&] {
            ordered = service.Subscribe([&](const GraphicsAdapterSnapshot& snapshot) {
                std::unique_lock lock(orderMutex);
                observed.push_back(snapshot);
                if (observed.size() == 1) {
                    initialEntered = true;
                    orderCv.notify_all();
                    orderCv.wait(lock, [&] { return releaseInitial; });
                }
            });
        });
        {
            std::unique_lock lock(orderMutex);
            Check(orderCv.wait_for(
                      lock, std::chrono::seconds{2}, [&] { return initialEntered; }),
                  "initial adapter callback entered before ordered publication");
        }
        const GraphicsAdapterSnapshot newer{
            .id = gameId,
            .dedicatedVideoMemoryBytes = 16ull << 30,
        };
        std::thread orderedPublisher([&] {
            GraphicsAdapterServiceTestAccess::ApplyCandidate(service, newer);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        {
            std::lock_guard lock(orderMutex);
            releaseInitial = true;
        }
        orderCv.notify_all();
        subscriber.join();
        orderedPublisher.join();
        {
            std::lock_guard lock(orderMutex);
            Check(observed.size() == 2 && observed.front() == GraphicsAdapterSnapshot{} &&
                      observed.back() == newer,
                  "subscriber observes initial and racing snapshots in publication order");
        }
        ordered.Reset();
    }

    // A callback can unsubscribe a later subscriber before it is invoked.
    {
        int firstCalls = 0;
        int cancelledCalls = 0;
        GraphicsAdapterService::Subscription later;
        auto first = service.Subscribe([&](const GraphicsAdapterSnapshot& snapshot) {
            if (snapshot.dedicatedVideoMemoryBytes == (4ull << 30)) {
                ++firstCalls;
                later.Reset();
            }
        });
        later = service.Subscribe([&](const GraphicsAdapterSnapshot& snapshot) {
            if (snapshot.dedicatedVideoMemoryBytes == (4ull << 30)) {
                ++cancelledCalls;
            }
        });
        GraphicsAdapterServiceTestAccess::ApplyCandidate(service, GraphicsAdapterSnapshot{
            .id = gameId,
            .dedicatedVideoMemoryBytes = 4ull << 30,
        });
        Check(firstCalls == 1 && cancelledCalls == 0,
              "adapter callback can unsubscribe a later callback safely");
        first.Reset();
    }

    // Unsubscribe is a barrier when publication is already executing the callback.
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    std::atomic<bool> resetReturned{false};
    auto slow = service.Subscribe([&](const GraphicsAdapterSnapshot& snapshot) {
        if (snapshot.dedicatedVideoMemoryBytes != (8ull << 30)) {
            return;
        }
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });

    std::thread publisher([&] {
        GraphicsAdapterServiceTestAccess::Publish(service, GraphicsAdapterSnapshot{
            .id = gameId,
            .dedicatedVideoMemoryBytes = 8ull << 30,
        });
    });
    {
        std::unique_lock lock(mutex);
        Check(cv.wait_for(lock, std::chrono::seconds{2}, [&] { return entered; }),
              "adapter callback entered before unsubscribe barrier");
    }
    std::thread resetter([&] {
        slow.Reset();
        resetReturned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    Check(!resetReturned.load(std::memory_order_acquire),
          "unsubscribe waits for an in-flight callback");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_all();
    publisher.join();
    resetter.join();
    Check(resetReturned.load(std::memory_order_acquire),
          "unsubscribe returns after the callback finishes");
    slow.Reset();

    // ReportAdapterId never performs DXGI work on the caller. Repeated reports
    // are coalesced while a transient failure is retried by the worker.
    {
        GraphicsAdapterServiceTestAccess::Reset(service);
        std::mutex probeMutex;
        std::condition_variable probeCv;
        int probeCalls = 0;
        bool firstEntered = false;
        bool releaseFirst = false;
        bool capacityPublished = false;
        GraphicsAdapterServiceTestAccess::SetProbe(
            service,
            [&](GraphicsAdapterId id) -> std::optional<GraphicsAdapterSnapshot> {
                std::unique_lock lock(probeMutex);
                ++probeCalls;
                probeCv.notify_all();
                if (probeCalls == 1) {
                    firstEntered = true;
                    probeCv.notify_all();
                    probeCv.wait(lock, [&] { return releaseFirst; });
                    return std::nullopt;
                }
                return GraphicsAdapterSnapshot{
                    .id = id,
                    .dedicatedVideoMemoryBytes = 12ull << 30,
                };
            });
        auto asyncSubscription = service.Subscribe([&](const GraphicsAdapterSnapshot& snapshot) {
            if (snapshot.dedicatedVideoMemoryBytes == (12ull << 30)) {
                std::lock_guard lock(probeMutex);
                capacityPublished = true;
                probeCv.notify_all();
            }
        });

        service.ReportAdapterId(gameId);
        {
            std::unique_lock lock(probeMutex);
            Check(probeCv.wait_for(
                      lock, std::chrono::seconds{2}, [&] { return firstEntered; }),
                  "asynchronous adapter probe starts after identity publication");
        }
        service.ReportAdapterId(gameId);
        service.ReportAdapterId(gameId);
        {
            std::lock_guard lock(probeMutex);
            Check(probeCalls == 1,
                  "same-LUID reports coalesce while a probe is in flight");
            releaseFirst = true;
        }
        probeCv.notify_all();
        {
            std::unique_lock lock(probeMutex);
            Check(probeCv.wait_for(
                      lock, std::chrono::seconds{2}, [&] { return capacityPublished; }),
                  "transient probe failure retries and eventually publishes VRAM");
            Check(probeCalls == 2, "successful retry completes without duplicate probes");
        }
        asyncSubscription.Reset();
    }

    // A LUID change supersedes an older in-flight result. The single worker
    // serializes probes and generation checking prevents stale publication.
    {
        GraphicsAdapterServiceTestAccess::Reset(service);
        constexpr GraphicsAdapterId oldId{.lowPart = 21, .highPart = 1};
        constexpr GraphicsAdapterId newId{.lowPart = 22, .highPart = 1};
        std::mutex probeMutex;
        std::condition_variable probeCv;
        bool oldEntered = false;
        bool releaseOld = false;
        std::atomic<int> oldCalls{0};
        std::atomic<int> newCalls{0};
        GraphicsAdapterServiceTestAccess::SetProbe(
            service,
            [&](GraphicsAdapterId id) -> std::optional<GraphicsAdapterSnapshot> {
                if (id == oldId) {
                    std::unique_lock lock(probeMutex);
                    oldCalls.fetch_add(1, std::memory_order_relaxed);
                    oldEntered = true;
                    probeCv.notify_all();
                    probeCv.wait(lock, [&] { return releaseOld; });
                    return GraphicsAdapterSnapshot{
                        .id = id,
                        .dedicatedVideoMemoryBytes = 24ull << 30,
                    };
                }
                {
                    std::lock_guard lock(probeMutex);
                    newCalls.fetch_add(1, std::memory_order_relaxed);
                    probeCv.notify_all();
                }
                return GraphicsAdapterSnapshot{
                    .id = id,
                    .dedicatedVideoMemoryBytes = 8ull << 30,
                };
            });

        service.ReportAdapterId(oldId);
        {
            std::unique_lock lock(probeMutex);
            Check(probeCv.wait_for(
                      lock, std::chrono::seconds{2}, [&] { return oldEntered; }),
                  "old adapter probe enters before identity replacement");
        }
        service.ReportAdapterId(newId);
        {
            std::lock_guard lock(probeMutex);
            releaseOld = true;
        }
        probeCv.notify_all();

        bool newPublished = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            const auto snapshot = service.Snapshot();
            if (snapshot.id == newId &&
                snapshot.dedicatedVideoMemoryBytes == (8ull << 30)) {
                newPublished = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        Check(newPublished && oldCalls.load(std::memory_order_relaxed) == 1 &&
                  newCalls.load(std::memory_order_relaxed) == 1,
              "new LUID wins and stale probe capacity is discarded");
    }

    // A successful UMA/software classification is complete even though the
    // public snapshot intentionally contains no dedicated-memory value.
    {
        GraphicsAdapterServiceTestAccess::Reset(service);
        std::atomic<int> probeCalls{0};
        GraphicsAdapterServiceTestAccess::SetProbe(
            service,
            [&](GraphicsAdapterId id) -> std::optional<GraphicsAdapterSnapshot> {
                probeCalls.fetch_add(1, std::memory_order_relaxed);
                return GraphicsAdapterSnapshot{.id = id};
            });
        service.ReportAdapterId(gameId);
        for (int attempt = 0;
             attempt < 200 && probeCalls.load(std::memory_order_relaxed) == 0;
             ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
        service.ReportAdapterId(gameId);
        std::this_thread::sleep_for(std::chrono::milliseconds{150});
        Check(probeCalls.load(std::memory_order_relaxed) == 1,
              "known no-dedicated result does not retry as a transient failure");
    }

    // Stop is a join barrier for an in-flight platform probe.
    {
        GraphicsAdapterServiceTestAccess::Reset(service);
        std::mutex probeMutex;
        std::condition_variable probeCv;
        bool probeEntered = false;
        bool releaseProbe = false;
        std::atomic<bool> stopFinished{false};
        GraphicsAdapterServiceTestAccess::SetProbe(
            service,
            [&](GraphicsAdapterId id) -> std::optional<GraphicsAdapterSnapshot> {
                std::unique_lock lock(probeMutex);
                probeEntered = true;
                probeCv.notify_all();
                probeCv.wait(lock, [&] { return releaseProbe; });
                return GraphicsAdapterSnapshot{.id = id};
            });
        service.ReportAdapterId(gameId);
        {
            std::unique_lock lock(probeMutex);
            Check(probeCv.wait_for(
                      lock, std::chrono::seconds{2}, [&] { return probeEntered; }),
                  "adapter probe enters before Stop barrier");
        }
        std::thread stopper([&] {
            service.Stop();
            stopFinished.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        Check(!stopFinished.load(std::memory_order_acquire),
              "GraphicsAdapterService::Stop waits for in-flight probe");
        {
            std::lock_guard lock(probeMutex);
            releaseProbe = true;
        }
        probeCv.notify_all();
        stopper.join();
        Check(stopFinished.load(std::memory_order_acquire),
              "GraphicsAdapterService::Stop returns after probe completion");
    }

    GraphicsAdapterServiceTestAccess::Reset(service);
}
