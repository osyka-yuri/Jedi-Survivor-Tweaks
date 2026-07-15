#include "core/streaming_pool_protocol.hpp"
#include "tweaks/streaming_pool_controller.hpp"
#include "tweaks/streaming_pool_policy.hpp"
#include "test_check.hpp"

#include <atomic>
#include <barrier>
#include <limits>
#include <thread>

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
};

} // namespace

void TestStreamingPoolController() {
    const auto p24 = MakePoolSizePolicy(GiB(24));
    const auto p8 = MakePoolSizePolicy(GiB(8));
    const auto p2 = MakePoolSizePolicy(GiB(2));

    // Bind publishes a coherent policy without selecting a mode.
    {
        BoundController bound(p24);
        const auto snapshot = bound.controller.Snapshot();
        Check(snapshot.state == StreamingPoolState::Unconfigured,
              "new bound controller remains unconfigured");
        Check(bound.payload.lockedBytes == 0,
              "binding does not create a streaming-pool lock");
        Check(bound.payload.captureCeilingBytes == PoolSizeGbToBytes(16.8f, p24.limits),
              "binding publishes the GPU-aware capture ceiling");
        Check(bound.payload.fallbackBytes == PoolSizeGbToBytes(2.0f, p24.limits),
              "binding publishes the GPU-aware fallback");
    }

    // Manual state retains the request but publishes its policy-normalized value.
    {
        BoundController bound(p24);
        bound.controller.ArmManual(20.0f);
        const auto snapshot = bound.controller.Snapshot();
        Check(snapshot.state == StreamingPoolState::Manual,
              "manual arm constructs Manual state");
        Check(NearlyEqual(snapshot.effectiveGb, 16.8f),
              "manual value is capped by current GPU policy");
        Check(NearlyEqual(snapshot.requestedManualGb, 20.0f),
              "manual request survives normalization");
        Check(bound.payload.lockedBytes == PoolSizeGbToBytes(16.8f, p24.limits),
              "manual state publishes its effective bytes");

        Check(bound.controller.UpdatePolicy(p8), "lower GPU policy is accepted");
        Check(NearlyEqual(bound.controller.Snapshot().effectiveGb, 5.6f),
              "manual state republishes immediately after policy reduction");
        Check(bound.controller.UpdatePolicy(p24), "higher GPU policy is accepted");
        Check(NearlyEqual(bound.controller.Snapshot().effectiveGb, 16.8f),
              "higher policy restores the retained manual request");
        Check(!bound.controller.UpdatePolicy(p24), "identical policy is deduplicated");
    }

    {
        BoundController bound(p24);
        bound.controller.ArmManual(std::numeric_limits<float>::infinity());
        const auto snapshot = bound.controller.Snapshot();
        Check(NearlyEqual(snapshot.requestedManualGb, p24.limits.FallbackGb()) &&
                  NearlyEqual(snapshot.effectiveGb, p24.limits.FallbackGb()),
              "non-finite manual request is sanitized to policy fallback");
    }

    // Auto CVar acquisition preserves the observed MiB exactly.
    {
        BoundController bound(p24);
        bound.controller.ArmAuto();
        Check(bound.controller.IsWaitingForEngine(), "auto begins in WaitingForEngine");
        Check(bound.controller.ObserveEnginePoolMb(0) == EnginePoolObservation::NotReady,
              "zero CVar is reported as not ready");
        Check(bound.controller.ObserveEnginePoolMb(3000) ==
                  EnginePoolObservation::LockedFromCVar,
              "valid CVar wins an open capture gate");
        const auto snapshot = bound.controller.Snapshot();
        const uint64_t exactBytes = 3000ull * jst::core::kBytesPerMiB;
        Check(snapshot.state == StreamingPoolState::LockedFromCVar,
              "CVar winner constructs LockedFromCVar state");
        Check(snapshot.enginePoolMb == 3000 && snapshot.lockedBytes == exactBytes,
              "CVar winner retains exact MB and byte values");
        Check(bound.payload.lockedBytes == exactBytes,
              "CVar winner publishes exact bytes without grid rounding");
        Check(bound.controller.ObserveEnginePoolMb(2048) == EnginePoolObservation::Inactive,
              "later CVar observations are inactive after a CVar lock");

        Check(bound.controller.UpdatePolicy(p24) == false,
              "unchanged policy does not disturb exact CVar lock");
        Check(bound.payload.lockedBytes == exactBytes,
              "exact CVar lock survives a no-op policy publication");
    }

    // Invalid candidates leave waiting state and expose a detailed rejection.
    {
        BoundController bound(p24);
        bound.controller.ArmAuto();
        Check(bound.controller.ObserveEnginePoolMb(100) ==
                  EnginePoolObservation::RejectedBelowMinimum,
              "sub-minimum CVar is rejected explicitly");
        Check(bound.controller.ObserveEnginePoolMb(20'000) ==
                  EnginePoolObservation::RejectedAboveMaximum,
              "over-ceiling CVar is rejected explicitly");
        const auto snapshot = bound.controller.Snapshot();
        Check(snapshot.state == StreamingPoolState::WaitingForEngine,
              "rejected CVar leaves controller waiting");
        Check(snapshot.lastRejectedCandidate &&
                  snapshot.lastRejectedCandidate->sizeMb == 20'000,
              "snapshot retains the latest rejected candidate");
        Check(bound.payload.lockedBytes == 0,
              "rejected observations cannot publish a lock");
    }

    // Timeout selects the safe policy fallback, including low-VRAM capping.
    {
        BoundController bound(p2);
        bound.controller.ArmAuto();
        bound.controller.OnAutoTimeout();
        const auto snapshot = bound.controller.Snapshot();
        Check(snapshot.state == StreamingPoolState::Fallback,
              "auto timeout constructs Fallback state");
        Check(NearlyEqual(snapshot.effectiveGb, 1.4f),
              "2 GiB GPU caps fallback to 1.4 GiB");
        Check(bound.payload.lockedBytes == PoolSizeGbToBytes(1.4f, p2.limits),
              "timeout publishes safe fallback bytes");
        bound.controller.OnAutoTimeout();
        Check(bound.controller.Snapshot().state == StreamingPoolState::Fallback,
              "timeout is idempotent outside waiting state");
    }

    // A streaming-path capture wins over CVar, while invalid payload is cleared.
    {
        BoundController bound(p8);
        bound.controller.ArmAuto();
        bound.payload.lockedBytes = 3ull * jst::core::kBytesPerGiB;
        Check(bound.controller.PullDetourIfWaiting(),
              "valid streaming-path payload is adopted");
        Check(bound.controller.Snapshot().state == StreamingPoolState::LockedFromDetour,
              "detour winner constructs LockedFromDetour state");
        Check(bound.controller.ObserveEnginePoolMb(3000) ==
                  EnginePoolObservation::LockedFromDetour,
              "CVar observation reports an existing detour winner");

        bound.controller.ArmAuto();
        bound.payload.lockedBytes = 8ull * jst::core::kBytesPerGiB;
        Check(!bound.controller.PullDetourIfWaiting(),
              "over-ceiling streaming-path payload is rejected");
        Check(bound.controller.Snapshot().state == StreamingPoolState::WaitingForEngine &&
                  bound.payload.lockedBytes == 0,
              "invalid detour lock is cleared while capture remains open");
    }

    // Install-order orphan is adopted exactly when auto mode is first armed.
    {
        BoundController bound(p24);
        const uint64_t orphan = 1537ull * jst::core::kBytesPerMiB;
        bound.payload.lockedBytes = orphan;
        bound.controller.ArmAuto();
        const auto snapshot = bound.controller.Snapshot();
        Check(snapshot.state == StreamingPoolState::LockedFromDetour,
              "ArmAuto adopts a valid pre-existing detour lock");
        Check(snapshot.lockedBytes == orphan && bound.payload.lockedBytes == orphan,
              "orphan adoption preserves its exact bytes");
    }

    // Mode toggles clear forced locks and preserve the requested manual value.
    {
        BoundController bound(p24);
        bound.controller.ArmManual(5.0f);
        bound.controller.ArmAuto();
        Check(bound.controller.Snapshot().state == StreamingPoolState::WaitingForEngine &&
                  bound.payload.lockedBytes == 0,
              "manual-to-auto reopens the engine capture gate");
        Check(!bound.controller.UpdateManualSize(7.0f),
              "editing retained manual value does not force auto mode");
        Check(NearlyEqual(bound.controller.Snapshot().requestedManualGb, 7.0f),
              "auto mode retains the edited manual request");
        bound.controller.ArmManual(bound.controller.Snapshot().requestedManualGb);
        Check(NearlyEqual(bound.controller.Snapshot().effectiveGb, 7.0f),
              "return to manual restores the retained request");
        Check(bound.controller.UpdateManualSize(6.0f),
              "manual edit republishes while Manual is active");
        Check(bound.payload.lockedBytes == PoolSizeGbToBytes(6.0f, p24.limits),
              "manual edit updates payload immediately");
    }

    // Policy changes preserve valid automatic locks and repair unsafe ones.
    {
        BoundController bound(p8);
        bound.controller.ArmAuto();
        Check(bound.controller.ObserveEnginePoolMb(3001) ==
                  EnginePoolObservation::LockedFromCVar,
              "non-grid CVar locks under 8 GiB policy");
        const uint64_t exact = 3001ull * jst::core::kBytesPerMiB;
        Check(bound.controller.UpdatePolicy(p24), "policy increase is published");
        Check(bound.controller.Snapshot().state == StreamingPoolState::LockedFromCVar &&
                  bound.payload.lockedBytes == exact,
              "policy increase preserves valid auto lock without rounding");
        Check(bound.controller.UpdatePolicy(p2), "policy reduction is published");
        Check(bound.controller.Snapshot().state == StreamingPoolState::Fallback,
              "policy reduction replaces unsafe auto lock with fallback");
        Check(bound.payload.lockedBytes == PoolSizeGbToBytes(1.4f, p2.limits),
              "unsafe auto lock is replaced by low-VRAM fallback");
    }

    // While waiting, policy publication closes the gate and accepts a valid winner.
    {
        BoundController bound(p24);
        bound.controller.ArmAuto();
        const uint64_t detour = 3ull * jst::core::kBytesPerGiB;
        bound.payload.lockedBytes = detour;
        Check(bound.controller.UpdatePolicy(p8), "waiting policy update is published");
        Check(bound.controller.Snapshot().state == StreamingPoolState::LockedFromDetour &&
                  bound.payload.lockedBytes == detour,
              "waiting policy update adopts a concurrent valid detour winner");

        bound.controller.ArmAuto();
        bound.payload.lockedBytes = 10ull * jst::core::kBytesPerGiB;
        Check(bound.controller.UpdatePolicy(p2), "second waiting policy update is published");
        Check(bound.controller.Snapshot().state == StreamingPoolState::Fallback &&
                  bound.payload.lockedBytes == PoolSizeGbToBytes(1.4f, p2.limits),
              "waiting policy update repairs an invalid detour winner");
    }

    // A simultaneous CVar/detour CAS has one coherent first winner.
    {
        BoundController bound(p24);
        bound.controller.ArmAuto();
        constexpr uint64_t detourBytes = 2049ull * jst::core::kBytesPerMiB;
        std::barrier start{3};
        std::thread detour([&] {
            start.arrive_and_wait();
            std::atomic_ref lock(bound.payload.lockedBytes);
            uint64_t expected = 0;
            (void)lock.compare_exchange_strong(expected, detourBytes);
        });
        EnginePoolObservation observation = EnginePoolObservation::Inactive;
        std::thread cvar([&] {
            start.arrive_and_wait();
            observation = bound.controller.ObserveEnginePoolMb(3000);
        });
        start.arrive_and_wait();
        detour.join();
        cvar.join();
        (void)bound.controller.PullDetourIfWaiting();

        const auto snapshot = bound.controller.Snapshot();
        const bool cvarWon = snapshot.state == StreamingPoolState::LockedFromCVar &&
                             snapshot.lockedBytes == 3000ull * jst::core::kBytesPerMiB;
        const bool detourWon = snapshot.state == StreamingPoolState::LockedFromDetour &&
                               snapshot.lockedBytes == detourBytes;
        Check(cvarWon || detourWon,
              "CVar/detour race produces exactly one valid typed winner");
        Check(bound.payload.lockedBytes == snapshot.lockedBytes,
              "race winner is consistent between snapshot and payload");
        Check(observation == EnginePoolObservation::LockedFromCVar ||
                  observation == EnginePoolObservation::LockedFromDetour,
              "race observation reports one of the two valid winners");
    }
}
