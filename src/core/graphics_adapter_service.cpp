#include "graphics_adapter_service.hpp"

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <utility>

#pragma comment(lib, "dxgi.lib")

namespace jst::core {

namespace {

constexpr uint64_t kMinimumDedicatedVideoMemoryBytes = 512ull << 20; // 512 MiB

[[nodiscard]] LUID ToLuid(GraphicsAdapterId id) noexcept {
    return LUID{
        .LowPart = id.lowPart,
        .HighPart = id.highPart,
    };
}

} // namespace

GraphicsAdapterService& GraphicsAdapterService::Instance() {
    static GraphicsAdapterService instance;
    return instance;
}

GraphicsAdapterService::GraphicsAdapterService()
    : m_probeFunction(&GraphicsAdapterService::ProbeAdapter) {}

GraphicsAdapterService::~GraphicsAdapterService() {
    Stop();
}

GraphicsAdapterSnapshot GraphicsAdapterService::ClassifyAdapter(
    GraphicsAdapterId id, uint64_t dedicatedBytes, bool software) noexcept {
    GraphicsAdapterSnapshot snapshot{.id = id};
    if (!software && dedicatedBytes >= kMinimumDedicatedVideoMemoryBytes) {
        snapshot.dedicatedVideoMemoryBytes = dedicatedBytes;
    }
    return snapshot;
}

GraphicsAdapterSnapshot GraphicsAdapterService::Snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_snapshot;
}

GraphicsAdapterService::ProbeResult
GraphicsAdapterService::ProbeAdapter(GraphicsAdapterId id) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return ProbeResult{
            .snapshot = GraphicsAdapterSnapshot{.id = id},
            .disposition = ProbeDisposition::Retry,
        };
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter4> adapter;
    if (FAILED(factory->EnumAdapterByLuid(ToLuid(id), IID_PPV_ARGS(&adapter)))) {
        return ProbeResult{
            .snapshot = GraphicsAdapterSnapshot{.id = id},
            .disposition = ProbeDisposition::Retry,
        };
    }

    DXGI_ADAPTER_DESC3 description{};
    if (FAILED(adapter->GetDesc3(&description))) {
        return ProbeResult{
            .snapshot = GraphicsAdapterSnapshot{.id = id},
            .disposition = ProbeDisposition::Retry,
        };
    }

    return ProbeResult{
        .snapshot = ClassifyAdapter(
            id,
            description.DedicatedVideoMemory,
            (description.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0),
        .disposition = ProbeDisposition::Complete,
    };
}

std::chrono::milliseconds GraphicsAdapterService::RetryDelay(uint32_t attempt) noexcept {
    using namespace std::chrono_literals;
    constexpr std::array<std::chrono::milliseconds, 6> delays{
        100ms, 500ms, 2s, 5s, 15s, 30s};
    return delays[(std::min)(static_cast<size_t>(attempt), delays.size() - 1)];
}

void GraphicsAdapterService::StartWorkerLocked() {
    if (m_probeThread.joinable()) {
        return;
    }
    m_probeStopRequested = false;
    m_probeThread = std::thread([this] { ProbeLoop(); });
}

void GraphicsAdapterService::Start() noexcept {
    try {
        std::lock_guard lifecycleLock(m_workerLifecycleMutex);
        {
            std::lock_guard lock(m_mutex);
            StartWorkerLocked();
        }
        m_probeCv.notify_all();
    } catch (...) {
        // A later adapter report retries worker creation. Until then consumers
        // retain the conservative legacy policy.
    }
}

void GraphicsAdapterService::Stop() {
    std::lock_guard lifecycleLock(m_workerLifecycleMutex);
    std::thread worker;
    {
        std::lock_guard lock(m_mutex);
        ++m_probeGeneration;
        m_pendingProbe.reset();
        m_probeStopRequested = true;
        if (m_probeThread.joinable()) {
            worker = std::move(m_probeThread);
        }
    }
    m_probeCv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
    {
        std::lock_guard lock(m_mutex);
        m_inFlightProbe.reset();
        m_probeStopRequested = false;
    }
}

void GraphicsAdapterService::ReportAdapterId(GraphicsAdapterId id) noexcept {
    std::vector<std::shared_ptr<Subscriber>> subscribers;
    GraphicsAdapterSnapshot publication;
    uint64_t sequence = 0;

    try {
        {
            std::lock_guard lock(m_mutex);
            if (m_probeStopRequested) {
                return;
            }

            const bool sameId = m_snapshot.id == id;
            const bool completed = sameId && m_completedProbeId == id;
            const bool hasCapacity = sameId && m_snapshot.HasDedicatedVideoMemory();
            const bool alreadyQueued =
                (m_pendingProbe && m_pendingProbe->id == id) ||
                (m_inFlightProbe && m_inFlightProbe->id == id);

            if (!sameId) {
                ++m_probeGeneration;
                m_completedProbeId.reset();
                m_pendingProbe = ProbeWork{
                    .id = id,
                    .generation = m_probeGeneration,
                    .attempt = 0,
                    .notBefore = std::chrono::steady_clock::now(),
                };

                publication = MergeCapacityLocked(GraphicsAdapterSnapshot{.id = id});
                m_snapshot = publication;
                sequence = ++m_sequence;
                subscribers = m_subscribers;
            } else if (completed) {
                return;
            } else if (!alreadyQueued && !hasCapacity) {
                ++m_probeGeneration;
                m_pendingProbe = ProbeWork{
                    .id = id,
                    .generation = m_probeGeneration,
                    .attempt = 0,
                    .notBefore = std::chrono::steady_clock::now(),
                };
            } else if (!alreadyQueued) {
                return;
            }

            // Starting a thread is only a fallback for reports that beat the
            // normal Application::Start path. DXGI work never runs here.
            StartWorkerLocked();
        }

    } catch (...) {
        // Never unwind through D3D12CreateDevice or a ReShade callback. A
        // later identity report can start the queued probe if thread creation
        // was temporarily unavailable.
    }

    m_probeCv.notify_all();
    if (sequence != 0) {
        for (const auto& subscriber : subscribers) {
            Invoke(subscriber, publication, sequence);
        }
    }
}

void GraphicsAdapterService::ProbeLoop() {
    std::unique_lock lock(m_mutex);
    while (!m_probeStopRequested) {
        m_probeCv.wait(lock, [this] {
            return m_probeStopRequested || m_pendingProbe.has_value();
        });
        if (m_probeStopRequested) {
            break;
        }

        const auto generation = m_pendingProbe->generation;
        const auto notBefore = m_pendingProbe->notBefore;
        if (std::chrono::steady_clock::now() < notBefore) {
            m_probeCv.wait_until(lock, notBefore, [this, generation, notBefore] {
                return m_probeStopRequested || !m_pendingProbe ||
                       m_pendingProbe->generation != generation ||
                       m_pendingProbe->notBefore != notBefore;
            });
            continue;
        }

        ProbeWork work = *m_pendingProbe;
        m_pendingProbe.reset();
        m_inFlightProbe = work;
        const ProbeFunction probe = m_probeFunction;
        lock.unlock();

        ProbeResult result{
            .snapshot = GraphicsAdapterSnapshot{.id = work.id},
            .disposition = ProbeDisposition::Retry,
        };
        try {
            if (probe) {
                result = probe(work.id);
            }
        } catch (...) {
            // A failed custom/platform probe follows the same retry path as a
            // transient HRESULT and cannot terminate the worker.
        }

        lock.lock();
        if (m_inFlightProbe && m_inFlightProbe->generation == work.generation) {
            m_inFlightProbe.reset();
        }
        if (m_probeStopRequested || work.generation != m_probeGeneration) {
            continue;
        }

        if (result.disposition == ProbeDisposition::Retry) {
            work.notBefore = std::chrono::steady_clock::now() + RetryDelay(work.attempt);
            ++work.attempt;
            m_pendingProbe = work;
            m_probeCv.notify_all();
            continue;
        }

        result.snapshot.id = work.id;
        result.snapshot = MergeCapacityLocked(std::move(result.snapshot));
        m_completedProbeId = work.id;
        if (m_snapshot == result.snapshot) {
            continue;
        }

        m_snapshot = result.snapshot;
        const uint64_t publicationSequence = ++m_sequence;
        const auto subscribers = m_subscribers;
        const auto publication = m_snapshot;
        lock.unlock();
        for (const auto& subscriber : subscribers) {
            Invoke(subscriber, publication, publicationSequence);
        }
        lock.lock();
    }
}

GraphicsAdapterSnapshot GraphicsAdapterService::MergeCapacityLocked(
    GraphicsAdapterSnapshot candidate) noexcept {
    if (!m_snapshot.HasDedicatedVideoMemory()) {
        m_capacitySourceId = candidate.HasDedicatedVideoMemory()
            ? candidate.id
            : std::nullopt;
        return candidate;
    }

    if (!candidate.HasDedicatedVideoMemory()) {
        candidate.dedicatedVideoMemoryBytes = m_snapshot.dedicatedVideoMemoryBytes;
        return candidate;
    }

    const bool sameSource = candidate.id && candidate.id == m_capacitySourceId;
    if (sameSource ||
        *candidate.dedicatedVideoMemoryBytes >= *m_snapshot.dedicatedVideoMemoryBytes) {
        m_capacitySourceId = candidate.id;
        return candidate;
    }

    candidate.dedicatedVideoMemoryBytes = m_snapshot.dedicatedVideoMemoryBytes;
    return candidate;
}

void GraphicsAdapterService::SetCapacitySourceLocked(
    const GraphicsAdapterSnapshot& snapshot) noexcept {
    m_capacitySourceId = snapshot.HasDedicatedVideoMemory()
        ? snapshot.id
        : std::nullopt;
}

void GraphicsAdapterService::ApplyCandidate(
    GraphicsAdapterSnapshot candidate,
    bool force) {
    std::vector<std::shared_ptr<Subscriber>> subscribers;
    uint64_t sequence = 0;
    GraphicsAdapterSnapshot publication{};
    {
        std::lock_guard lock(m_mutex);
        if (force) {
            SetCapacitySourceLocked(candidate);
        } else {
            candidate = MergeCapacityLocked(std::move(candidate));
        }
        if (!force && m_snapshot == candidate) {
            return;
        }

        ++m_probeGeneration;
        m_pendingProbe.reset();
        m_completedProbeId = candidate.id;
        if (m_snapshot == candidate) {
            m_probeCv.notify_all();
            return;
        }
        m_snapshot = candidate;
        publication = candidate;
        sequence = ++m_sequence;
        subscribers = m_subscribers;
    }
    m_probeCv.notify_all();

    for (const auto& subscriber : subscribers) {
        Invoke(subscriber, publication, sequence);
    }
}

void GraphicsAdapterService::Invoke(
    const std::shared_ptr<Subscriber>& subscriber,
    const GraphicsAdapterSnapshot& snapshot,
    uint64_t sequence) {
    std::lock_guard invokeLock(subscriber->invokeMutex);
    Callback callback;
    {
        std::lock_guard lock(subscriber->mutex);
        if (!subscriber->active ||
            (subscriber->lastSequence && *subscriber->lastSequence >= sequence)) {
            return;
        }
        ++subscriber->inFlight;
        subscriber->lastSequence = sequence;
        callback = subscriber->callback;
    }

    try {
        callback(snapshot);
    } catch (...) {
        // A consumer must never unwind through a graphics callback or stop the
        // adapter worker.
    }

    {
        std::lock_guard lock(subscriber->mutex);
        --subscriber->inFlight;
        subscriber->cv.notify_all();
    }
}

GraphicsAdapterService::Subscription GraphicsAdapterService::Subscribe(Callback callback) {
    if (!callback) {
        return {};
    }

    auto subscriber = std::make_shared<Subscriber>();
    subscriber->callback = std::move(callback);

    GraphicsAdapterSnapshot snapshot;
    uint64_t sequence = 0;
    {
        std::lock_guard lock(m_mutex);
        m_subscribers.push_back(subscriber);
        snapshot = m_snapshot;
        sequence = m_sequence;
    }
    Invoke(subscriber, snapshot, sequence);
    return Subscription(this, std::move(subscriber));
}

void GraphicsAdapterService::Unsubscribe(
    const std::shared_ptr<Subscriber>& subscriber) {
    if (!subscriber) {
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        std::erase(m_subscribers, subscriber);
    }

    std::unique_lock lock(subscriber->mutex);
    subscriber->active = false;
    subscriber->cv.wait(lock, [&] { return subscriber->inFlight == 0; });
}

GraphicsAdapterService::Subscription::~Subscription() {
    Reset();
}

GraphicsAdapterService::Subscription::Subscription(Subscription&& other) noexcept
    : m_owner(std::exchange(other.m_owner, nullptr)),
      m_subscriber(std::move(other.m_subscriber)) {}

GraphicsAdapterService::Subscription&
GraphicsAdapterService::Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        Reset();
        m_owner = std::exchange(other.m_owner, nullptr);
        m_subscriber = std::move(other.m_subscriber);
    }
    return *this;
}

void GraphicsAdapterService::Subscription::Reset() {
    if (m_owner) {
        m_owner->Unsubscribe(m_subscriber);
    }
    m_subscriber.reset();
    m_owner = nullptr;
}

} // namespace jst::core
