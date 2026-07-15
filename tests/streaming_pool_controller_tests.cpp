#include "core/streaming_pool_protocol.hpp"
#include "tweaks/streaming_pool_controller.hpp"
#include "tweaks/streaming_pool_policy.hpp"
#include "test_check.hpp"

#include <atomic>
#include <barrier>
#include <limits>
#include <thread>
#include <utility>

using jst::tweaks::EnginePoolObservation;
using jst::tweaks::MakePoolSizePolicy;
using jst::tweaks::PoolSizeGbToBytes;
using jst::tweaks::StreamingPoolController;
using jst::tweaks::StreamingPoolState;

namespace {

constexpr uint64_t GiB(uint64_t value) {
    return value * jst::core::kBytesPerGiB;
}

struct BoundController {
    jst::core::StreamingPoolPayload payload{};
    StreamingPoolController controller;

    explicit BoundController(jst::tweaks::PoolSizePolicy policy = {})
        : controller(std::move(policy)) {
        controller.BindPayload(payload);
    }

    [[nodiscard]] uint64_t ForcedBytes() {
        return std::atomic_ref<uint64_t>(payload.forcedBytes)
            .load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t CeilingBytes() {
        return std::atomic_ref<uint64_t>(payload.captureCeilingBytes)
            .load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t FallbackBytes() {
        return std::atomic_ref<uint64_t>(payload.fallbackBytes)
            .load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t FirstObservedBytes() {
        return std::atomic_ref<uint64_t>(payload.firstObservedEngineBytes)
            .load(std::memory_order_acquire);
    }

    bool PublishFirstObserved(uint64_t bytes) {
        uint64_t expected = 0;
        return std::atomic_ref<uint64_t>(payload.firstObservedEngineBytes)
            .compare_exchange_strong(
                expected, bytes, std::memory_order_acq_rel, std::memory_order_acquire);
    }
};

void CheckPayloadMatchesSnapshot(BoundController& bound, const char* message) {
    Check(bound.ForcedBytes() == bound.controller.Snapshot().lockedBytes, message);
}

} // namespace

void TestStreamingPoolController() {
    const auto p24 = MakePoolSizePolicy(GiB(24));
    const auto p8 = MakePoolSizePolicy(GiB(8));
    const auto p2 = MakePoolSizePolicy(GiB(2));

    // Binding publishes policy without choosing a mode or forcing a size.
    {
        BoundController bound(p24);
        const auto snapshot = bound.controller.Snapshot();
        Check(snapshot.state == StreamingPoolState::Unconfigured,
              "new bound controller remains unconfigured");
        Check(bound.ForcedBytes() == 0, "binding leaves the forced size open");
        Check(bound.CeilingBytes() == p24.limits.maximumBytes,
              "binding publishes the GPU-aware capture ceiling");
        Check(bound.FallbackBytes() == p24.limits.fallbackBytes,
              "binding publishes the GPU-aware fallback");
        Check(bound.FirstObservedBytes() == 0,
              "binding does not manufacture an engine path sample");
    }

    // Manual state retains the request while publishing its normalized value.
    {
        BoundController bound(p24);
        bound.controller.ArmManual(20.0f);
        auto snapshot = bound.controller.Snapshot();
        Check(snapshot.state == StreamingPoolState::Manual,
              "manual arm constructs Manual state");
        Check(NearlyEqual(snapshot.effectiveGb, 16.8f) &&
                  NearlyEqual(snapshot.requestedManualGb, 20.0f),
              "manual request is retained after GPU-policy normalization");
        CheckPayloadMatchesSnapshot(bound, "manual state publishes its effective bytes");

        Check(bound.controller.UpdatePolicy(p8), "lower manual policy is accepted");
        Check(NearlyEqual(bound.controller.Snapshot().effectiveGb, 5.6f),
              "lower policy immediately caps the manual lock");
        CheckPayloadMatchesSnapshot(bound, "lower manual policy updates the payload");

        Check(bound.controller.UpdatePolicy(p24), "higher manual policy is accepted");
        Check(NearlyEqual(bound.controller.Snapshot().effectiveGb, 16.8f),
              "higher policy restores the retained manual request");
        const auto beforeNoOp = bound.ForcedBytes();
        Check(!bound.controller.UpdatePolicy(p24), "identical policy is deduplicated");
        Check(bound.ForcedBytes() == beforeNoOp,
              "no-op policy leaves the forced value unchanged");

        Check(bound.controller.UpdateManualSize(6.0f),
              "manual edit republishes while Manual is active");
        Check(bound.ForcedBytes() == PoolSizeGbToBytes(6.0f, p24.limits),
              "manual edit updates the forced payload immediately");
    }

    {
        BoundController bound(p24);
        bound.controller.ArmManual(std::numeric_limits<float>::infinity());
        const auto snapshot = bound.controller.Snapshot();
        Check(NearlyEqual(snapshot.requestedManualGb, p24.limits.FallbackGb()) &&
                  NearlyEqual(snapshot.effectiveGb, p24.limits.FallbackGb()),
              "non-finite manual request is sanitized to policy fallback");
    }

    // Auto waits behind a safe hold and gives an in-range CVar first priority.
    {
        BoundController bound(p24);
        bound.controller.ArmManual(2.0f);
        Check(bound.PublishFirstObserved(3ull * jst::core::kBytesPerGiB),
              "test path sample wins the empty observation slot");
        bound.controller.ArmAuto();

        const auto waiting = bound.controller.Snapshot();
        Check(waiting.state == StreamingPoolState::WaitingForEngine,
              "ArmAuto waits for the CVar even with an existing path sample");
        Check(bound.ForcedBytes() == PoolSizeGbToBytes(2.0f, p24.limits),
              "Auto keeps the previous safe manual size as its closed hold");

        Check(bound.controller.ObserveEnginePoolMb(4000) ==
                  EnginePoolObservation::LockedFromCVar,
              "in-range CVar wins over a pre-existing path sample");
        const auto locked = bound.controller.Snapshot();
        Check(locked.state == StreamingPoolState::LockedFromCVar &&
                  locked.enginePoolMb == 4000 &&
                  locked.lockedBytes == 4000ull * jst::core::kBytesPerMiB,
              "CVar lock preserves the exact engine MiB value");
        Check(bound.FirstObservedBytes() == 3ull * jst::core::kBytesPerGiB,
              "CVar selection does not clear the process-lifetime path sample");
        CheckPayloadMatchesSnapshot(bound, "CVar winner is coherent with the payload");
        Check(bound.controller.ObserveEnginePoolMb(2048) == EnginePoolObservation::Inactive,
              "later CVar observations are inactive after a CVar lock");
    }

    // Invalid CVar values keep the safe hold and expose the latest rejection.
    {
        BoundController bound(p24);
        bound.controller.ArmAuto();
        const auto temporary = bound.ForcedBytes();
        Check(bound.controller.ObserveEnginePoolMb(0) == EnginePoolObservation::NotReady,
              "zero CVar is reported as not ready");
        Check(!bound.controller.TryAdoptPathSample(),
              "missing path sample cannot resolve Auto");
        Check(bound.controller.ObserveEnginePoolMb(100) ==
                  EnginePoolObservation::RejectedBelowMinimum,
              "sub-minimum CVar is rejected explicitly");
        Check(bound.controller.ObserveEnginePoolMb(20'000) ==
                  EnginePoolObservation::RejectedAboveMaximum,
              "over-ceiling CVar is rejected explicitly");
        const auto snapshot = bound.controller.Snapshot();
        Check(snapshot.state == StreamingPoolState::WaitingForEngine &&
                  snapshot.lastRejectedCandidate &&
                  snapshot.lastRejectedCandidate->sizeMb == 20'000,
              "rejected CVar leaves Auto waiting with rejection diagnostics");
        Check(bound.ForcedBytes() == temporary,
              "invalid CVar observations preserve the closed hold");
    }

    // A path sample is secondary after an unusable CVar tick.
    {
        BoundController bound(p24);
        bound.controller.ArmAuto();
        Check(bound.controller.ObserveEnginePoolMb(7'471'215) ==
                  EnginePoolObservation::RejectedAboveMaximum,
              "absurd CVar is rejected before considering the path sample");
        const uint64_t sample = 3000ull * jst::core::kBytesPerMiB;
        Check(bound.PublishFirstObserved(sample), "path publishes its first valid sample");
        Check(bound.controller.TryAdoptPathSample(),
              "path sample resolves Auto after an unusable CVar");
        Check(bound.controller.Snapshot().state ==
                  StreamingPoolState::LockedFromPathSample &&
                  bound.ForcedBytes() == sample,
              "path sample becomes the exact forced lock");
        Check(bound.controller.ObserveEnginePoolMb(3000) ==
                  EnginePoolObservation::LockedFromPathSample,
              "later CVar tick reports an existing path-sample winner");
    }

    // Timeout adopts a safe sample, otherwise it publishes the policy fallback.
    {
        BoundController bound(p2);
        bound.controller.ArmAuto();
        bound.controller.OnAutoTimeout();
        auto snapshot = bound.controller.Snapshot();
        Check(snapshot.state == StreamingPoolState::Fallback &&
                  NearlyEqual(snapshot.effectiveGb, 1.4f),
              "timeout without a sample publishes low-VRAM fallback");
        CheckPayloadMatchesSnapshot(bound, "timeout fallback is coherent with the payload");
        bound.controller.OnAutoTimeout();
        const auto afterSecondTimeout = bound.controller.Snapshot();
        Check(afterSecondTimeout.state == snapshot.state &&
                  afterSecondTimeout.lockedBytes == snapshot.lockedBytes &&
                  NearlyEqual(afterSecondTimeout.effectiveGb, snapshot.effectiveGb),
              "timeout is idempotent outside the waiting state");
    }

    {
        BoundController bound(p24);
        bound.controller.ArmManual(5.0f);
        bound.controller.ArmAuto();
        Check(bound.PublishFirstObserved(4ull * jst::core::kBytesPerGiB),
              "timeout scenario publishes a path sample");
        bound.controller.OnAutoTimeout();
        Check(bound.controller.Snapshot().state ==
                  StreamingPoolState::LockedFromPathSample &&
                  bound.ForcedBytes() == 4ull * jst::core::kBytesPerGiB,
              "timeout adopts a valid path sample before fallback");
    }

    {
        BoundController bound(p8);
        bound.controller.ArmAuto();
        Check(bound.PublishFirstObserved(8ull * jst::core::kBytesPerGiB),
              "test publishes an over-policy path sample");
        bound.controller.OnAutoTimeout();
        Check(bound.controller.Snapshot().state == StreamingPoolState::Fallback &&
                  bound.ForcedBytes() == p8.limits.fallbackBytes,
              "timeout rejects an out-of-range path sample");
        Check(bound.FirstObservedBytes() == 8ull * jst::core::kBytesPerGiB,
              "rejected process-lifetime sample remains immutable");
    }

    // Mode toggles retain manual intent without opening the forced gate.
    {
        BoundController bound(p24);
        bound.controller.ArmManual(5.0f);
        bound.controller.ArmAuto();
        Check(bound.ForcedBytes() == PoolSizeGbToBytes(5.0f, p24.limits),
              "manual-to-auto keeps the prior safe value as its hold");
        Check(!bound.controller.UpdateManualSize(7.0f),
              "editing retained manual value does not leave Auto");
        Check(NearlyEqual(bound.controller.Snapshot().requestedManualGb, 7.0f),
              "Auto retains the edited manual request");
        bound.controller.ArmManual(bound.controller.Snapshot().requestedManualGb);
        Check(NearlyEqual(bound.controller.Snapshot().effectiveGb, 7.0f),
              "return to Manual restores the retained request");
    }

    // Policy changes preserve valid locks and repair unsafe states.
    {
        BoundController bound(p24);
        Check(bound.controller.UpdatePolicy(p8),
              "unconfigured controller accepts a new GPU policy");
        Check(bound.controller.Snapshot().state == StreamingPoolState::Unconfigured &&
                  bound.ForcedBytes() == 0 &&
                  bound.CeilingBytes() == p8.limits.maximumBytes &&
                  bound.FallbackBytes() == p8.limits.fallbackBytes,
              "unconfigured policy update keeps the gate open and republishes policy");
    }

    {
        BoundController bound(p8);
        bound.controller.ArmAuto();
        Check(bound.controller.ObserveEnginePoolMb(3001) ==
                  EnginePoolObservation::LockedFromCVar,
              "non-grid CVar locks under the 8 GiB policy");
        const uint64_t exact = 3001ull * jst::core::kBytesPerMiB;
        Check(bound.controller.UpdatePolicy(p24), "policy increase is published");
        Check(bound.controller.Snapshot().state == StreamingPoolState::LockedFromCVar &&
                  bound.ForcedBytes() == exact,
              "policy increase preserves an exact valid CVar lock");
        Check(bound.controller.UpdatePolicy(p2), "policy reduction is published");
        Check(bound.controller.Snapshot().state == StreamingPoolState::Fallback &&
                  bound.ForcedBytes() == p2.limits.fallbackBytes,
              "policy reduction replaces an unsafe CVar lock with fallback");
    }

    {
        BoundController bound(p8);
        Check(bound.PublishFirstObserved(3ull * jst::core::kBytesPerGiB),
              "path-policy scenario publishes a valid sample");
        bound.controller.ArmAuto();
        Check(bound.controller.ObserveEnginePoolMb(-1) ==
                  EnginePoolObservation::NotReady &&
                  bound.controller.TryAdoptPathSample(),
              "invalid CVar allows the path sample to lock Auto");
        Check(bound.controller.UpdatePolicy(p24), "path lock accepts a higher policy");
        Check(bound.controller.Snapshot().state ==
                  StreamingPoolState::LockedFromPathSample &&
                  bound.ForcedBytes() == 3ull * jst::core::kBytesPerGiB,
              "higher policy preserves a valid path-sample lock");
        Check(bound.controller.UpdatePolicy(p2), "path lock accepts a lower policy");
        Check(bound.controller.Snapshot().state == StreamingPoolState::Fallback &&
                  bound.ForcedBytes() == p2.limits.fallbackBytes,
              "lower policy replaces an unsafe path-sample lock");
    }

    {
        BoundController bound(p24);
        bound.controller.ArmManual(5.0f);
        bound.controller.ArmAuto();
        Check(bound.controller.UpdatePolicy(p2),
              "waiting Auto accepts a lower GPU policy");
        Check(bound.controller.Snapshot().state == StreamingPoolState::WaitingForEngine &&
                  bound.ForcedBytes() == p2.limits.fallbackBytes,
              "waiting policy reduction repairs an out-of-range hold");
        Check(bound.CeilingBytes() == p2.limits.maximumBytes &&
                  bound.FallbackBytes() == p2.limits.fallbackBytes,
              "waiting policy reduction republishes both policy words");
    }

    {
        BoundController bound(p2);
        bound.controller.ArmAuto();
        bound.controller.OnAutoTimeout();
        Check(bound.controller.UpdatePolicy(p24), "fallback accepts a higher policy");
        Check(bound.controller.Snapshot().state == StreamingPoolState::Fallback &&
                  bound.ForcedBytes() == p24.limits.fallbackBytes,
              "Fallback state republishes the new policy fallback");
    }

    {
        StreamingPoolController controller(p24);
        controller.ArmAuto();
        Check(controller.ObserveEnginePoolMb(3000) ==
                  EnginePoolObservation::LockedFromCVar,
              "unbound controller can resolve Auto from a valid CVar");
        jst::core::StreamingPoolPayload payload{};
        controller.BindPayload(payload);
        Check(std::atomic_ref<uint64_t>(payload.forcedBytes)
                      .load(std::memory_order_acquire) ==
                  3000ull * jst::core::kBytesPerMiB,
              "late binding publishes the resolved controller lock");
    }

    // Concurrent detour writers can publish exactly one process-lifetime sample.
    {
        BoundController bound(p24);
        constexpr uint64_t first = 2049ull * jst::core::kBytesPerMiB;
        constexpr uint64_t second = 3073ull * jst::core::kBytesPerMiB;
        bool firstWon = false;
        bool secondWon = false;
        std::barrier start{3};
        std::thread firstWriter([&] {
            start.arrive_and_wait();
            firstWon = bound.PublishFirstObserved(first);
        });
        std::thread secondWriter([&] {
            start.arrive_and_wait();
            secondWon = bound.PublishFirstObserved(second);
        });
        start.arrive_and_wait();
        firstWriter.join();
        secondWriter.join();

        const uint64_t winner = bound.FirstObservedBytes();
        Check(firstWon != secondWon && (winner == first || winner == second),
              "concurrent path sampling has exactly one valid first writer");
        Check(!bound.PublishFirstObserved(4ull * jst::core::kBytesPerGiB) &&
                  bound.FirstObservedBytes() == winner,
              "later path samples cannot overwrite the first observation");

        bound.controller.ArmAuto();
        Check(bound.controller.ObserveEnginePoolMb(-1) ==
                  EnginePoolObservation::NotReady,
              "invalid CVar leaves the sampled Auto source available");
        Check(bound.controller.TryAdoptPathSample() && bound.ForcedBytes() == winner,
              "controller adopts the immutable concurrent sample exactly");
    }
}
