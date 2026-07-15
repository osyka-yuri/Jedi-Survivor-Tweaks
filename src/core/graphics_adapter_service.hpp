#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace jst::core {

#if defined(JST_UNIT_TESTS)
class GraphicsAdapterServiceTestAccess;
#endif

struct GraphicsAdapterId {
    uint32_t lowPart = 0;
    int32_t highPart = 0;

    bool operator==(const GraphicsAdapterId&) const = default;
};

struct GraphicsAdapterSnapshot {
    std::optional<GraphicsAdapterId> id;
    std::optional<uint64_t> dedicatedVideoMemoryBytes;

    [[nodiscard]] bool HasDedicatedVideoMemory() const noexcept {
        return dedicatedVideoMemoryBytes.has_value();
    }

    bool operator==(const GraphicsAdapterSnapshot&) const = default;
};

// Process-wide, loader-neutral identity and physical dedicated-memory service
// for the adapter actually selected by the game. Identity publication is
// synchronous and cheap; DXGI probing and retry run on one coalescing worker.
class GraphicsAdapterService final {
private:
    struct Subscriber;

public:
    using Callback = std::function<void(const GraphicsAdapterSnapshot&)>;

    class Subscription final {
    public:
        Subscription() = default;
        ~Subscription();

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;

        // Join barrier. Must not be called by this subscription's own callback.
        void Reset();
        [[nodiscard]] explicit operator bool() const noexcept { return m_owner != nullptr; }

    private:
        friend class GraphicsAdapterService;
        Subscription(GraphicsAdapterService* owner, std::shared_ptr<Subscriber> subscriber)
            : m_owner(owner), m_subscriber(std::move(subscriber)) {}

        GraphicsAdapterService* m_owner = nullptr;
        std::shared_ptr<Subscriber> m_subscriber;
    };

    [[nodiscard]] static GraphicsAdapterService& Instance();
    [[nodiscard]] GraphicsAdapterSnapshot Snapshot() const;

    void Start() noexcept;
    void Stop();
    void ReportAdapterId(GraphicsAdapterId id) noexcept;
    [[nodiscard]] Subscription Subscribe(Callback callback);

private:
    enum class ProbeDisposition {
        Complete,
        Retry,
    };

    struct ProbeResult {
        GraphicsAdapterSnapshot snapshot;
        ProbeDisposition disposition = ProbeDisposition::Retry;
    };

    struct ProbeWork {
        GraphicsAdapterId id;
        uint64_t generation = 0;
        uint32_t attempt = 0;
        std::chrono::steady_clock::time_point notBefore{};
    };

    using ProbeFunction = std::function<ProbeResult(GraphicsAdapterId)>;

    GraphicsAdapterService();
    ~GraphicsAdapterService();

    GraphicsAdapterService(const GraphicsAdapterService&) = delete;
    GraphicsAdapterService& operator=(const GraphicsAdapterService&) = delete;

    struct Subscriber {
        // Serializes publications for this subscriber without holding the
        // service mutex across consumer work.
        std::mutex invokeMutex;
        std::mutex mutex;
        std::condition_variable cv;
        Callback callback;
        bool active = true;
        size_t inFlight = 0;
        std::optional<uint64_t> lastSequence;
    };

    [[nodiscard]] static ProbeResult ProbeAdapter(GraphicsAdapterId id);
    [[nodiscard]] static GraphicsAdapterSnapshot ClassifyAdapter(
        GraphicsAdapterId id, uint64_t dedicatedBytes, bool software) noexcept;
    [[nodiscard]] static std::chrono::milliseconds RetryDelay(uint32_t attempt) noexcept;

    void StartWorkerLocked();
    void ProbeLoop();
    void ApplyCandidate(GraphicsAdapterSnapshot candidate, bool force = false);
    static void Invoke(
        const std::shared_ptr<Subscriber>& subscriber,
        const GraphicsAdapterSnapshot& snapshot,
        uint64_t sequence);
    void Unsubscribe(const std::shared_ptr<Subscriber>& subscriber);

    mutable std::mutex m_mutex;
    std::mutex m_workerLifecycleMutex;
    std::condition_variable m_probeCv;
    GraphicsAdapterSnapshot m_snapshot;
    std::optional<GraphicsAdapterId> m_completedProbeId;
    std::optional<ProbeWork> m_pendingProbe;
    std::optional<ProbeWork> m_inFlightProbe;
    uint64_t m_sequence = 0;
    uint64_t m_probeGeneration = 0;
    bool m_probeStopRequested = false;
    std::thread m_probeThread;
    ProbeFunction m_probeFunction;
    std::vector<std::shared_ptr<Subscriber>> m_subscribers;

#if defined(JST_UNIT_TESTS)
    friend class GraphicsAdapterServiceTestAccess;
#endif
};

} // namespace jst::core
