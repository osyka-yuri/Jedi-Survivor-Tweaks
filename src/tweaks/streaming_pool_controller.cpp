#include "streaming_pool_controller.hpp"

#include "slider_utils.hpp"

#include <atomic>
#include <cmath>
#include <format>
#include <utility>

namespace jst::tweaks {

static_assert(std::atomic_ref<uint64_t>::is_always_lock_free,
              "MASM payload protocol requires lock-free 64-bit atomics");

std::string FormatStreamingPoolStatus(const StreamingPoolSnapshot& snapshot) {
    std::string status;
    switch (snapshot.state) {
    case StreamingPoolState::Unconfigured:
        status = "Not configured";
        break;
    case StreamingPoolState::Manual:
        status = std::format("Manual: {:.1f} GB", snapshot.effectiveGb);
        if (!SliderValuesNearlyEqual(snapshot.effectiveGb, snapshot.requestedManualGb)) {
            status += std::format(" (requested {:.1f} GB)", snapshot.requestedManualGb);
        }
        break;
    case StreamingPoolState::WaitingForEngine:
        status = "Waiting for engine pool size...";
        break;
    case StreamingPoolState::LockedFromCVar:
        status = std::format("Locked to engine: {:.2f} GB ({} MB)",
                             snapshot.effectiveGb, snapshot.enginePoolMb);
        break;
    case StreamingPoolState::LockedFromDetour:
        status = std::format("Locked to engine: {:.2f} GB (streaming path)",
                             snapshot.effectiveGb);
        break;
    case StreamingPoolState::Fallback:
        status = std::format("Auto fallback: {:.1f} GB", snapshot.effectiveGb);
        break;
    }

    if (snapshot.lastRejectedCandidate) {
        const char* reason =
            snapshot.lastRejectedCandidate->reason == EnginePoolCandidateValidity::BelowMinimum
                ? "below safe minimum"
                : "above safe maximum";
        status += std::format(" | rejected {} MB {}",
                              snapshot.lastRejectedCandidate->sizeMb, reason);
    }

    if (snapshot.policy.dedicatedVideoMemoryBytes) {
        status += std::format(" | GPU {:.1f} GB, safe max {:.1f} GB",
                              PoolSizeBytesToGb(*snapshot.policy.dedicatedVideoMemoryBytes),
                              snapshot.policy.limits.MaximumGb());
    } else {
        status += " | VRAM unavailable; legacy 12.0 GB ceiling";
    }
    return status;
}

StreamingPoolController::StreamingPoolController(PoolSizePolicy policy)
    : m_policy(std::move(policy)),
      m_effectiveGb(m_policy.limits.FallbackGb()),
      m_requestedManualGb(m_policy.limits.FallbackGb()) {}

void StreamingPoolController::PayloadPort::Bind(
    jst::core::StreamingPoolPayload& payload) noexcept {
    m_payload = &payload;
}

uint64_t StreamingPoolController::PayloadPort::LoadLock(uint64_t fallback) const noexcept {
    if (!m_payload) {
        return fallback;
    }
    return std::atomic_ref<uint64_t>(m_payload->lockedBytes)
        .load(std::memory_order_acquire);
}

void StreamingPoolController::PayloadPort::StoreLock(uint64_t value) const noexcept {
    if (m_payload) {
        std::atomic_ref<uint64_t>(m_payload->lockedBytes)
            .store(value, std::memory_order_release);
    }
}

bool StreamingPoolController::PayloadPort::CompareExchangeLock(
    uint64_t& expected, uint64_t desired) const noexcept {
    if (!m_payload) {
        return false;
    }
    return std::atomic_ref<uint64_t>(m_payload->lockedBytes).compare_exchange_strong(
        expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
}

void StreamingPoolController::PayloadPort::PublishPolicy(
    uint64_t ceiling, uint64_t fallback) const noexcept {
    if (!m_payload) {
        return;
    }
    std::atomic_ref<uint64_t>(m_payload->captureCeilingBytes)
        .store(ceiling, std::memory_order_release);
    std::atomic_ref<uint64_t>(m_payload->fallbackBytes)
        .store(fallback, std::memory_order_release);
}

uint64_t StreamingPoolController::CeilingBytesLocked() const noexcept {
    return m_policy.limits.maximumBytes;
}

uint64_t StreamingPoolController::FallbackBytesLocked() const noexcept {
    return m_policy.limits.fallbackBytes;
}

void StreamingPoolController::PublishPolicyLocked() const noexcept {
    m_payload.PublishPolicy(CeilingBytesLocked(), FallbackBytesLocked());
}

void StreamingPoolController::EnterWaitingLocked() {
    PublishPolicyLocked();
    m_lockedBytes = 0;
    m_effectiveGb = m_policy.limits.FallbackGb();
    m_enginePoolMb = 0;
    m_lastRejectedCandidate.reset();
    m_state = StreamingPoolState::WaitingForEngine;
    m_payload.StoreLock(0);
}

void StreamingPoolController::PublishLockLocked(
    uint64_t bytes, StreamingPoolState state, int32_t engineMb) {
    m_lockedBytes = bytes;
    m_effectiveGb = PoolSizeBytesToGb(bytes);
    m_enginePoolMb = engineMb;
    m_lastRejectedCandidate.reset();
    m_state = state;
    m_payload.StoreLock(bytes);
}

void StreamingPoolController::AdoptDetourLocked(uint64_t bytes) {
    PublishLockLocked(bytes, StreamingPoolState::LockedFromDetour);
}

void StreamingPoolController::PublishFallbackLocked() {
    PublishLockLocked(FallbackBytesLocked(), StreamingPoolState::Fallback);
}

StreamingPoolController::AutoReconcileResult
StreamingPoolController::ReconcileOpenAutoPolicyLocked() {
    if (!m_payload.IsBound()) {
        PublishPolicyLocked();
        return AutoReconcileResult::Waiting;
    }

    const uint64_t fallback = FallbackBytesLocked();
    uint64_t observed = 0;
    for (;;) {
        // Temporarily force a safe value while replacing the policy words.
        // If this succeeds no detour capture can race the publication.
        if (observed == 0 && m_payload.CompareExchangeLock(observed, fallback)) {
            PublishPolicyLocked();
            m_payload.StoreLock(0);
            return AutoReconcileResult::Waiting;
        }

        if (observed != 0 && IsPoolSizeWithinLimits(observed, m_policy.limits)) {
            PublishPolicyLocked();
            AdoptDetourLocked(observed);
            return AutoReconcileResult::Captured;
        }

        if (observed != 0) {
            uint64_t expected = observed;
            if (m_payload.CompareExchangeLock(expected, fallback)) {
                PublishPolicyLocked();
                PublishFallbackLocked();
                return AutoReconcileResult::Fallback;
            }
            observed = expected;
            continue;
        }

        observed = m_payload.LoadLock(0);
    }
}

bool StreamingPoolController::TryAdoptLiveDetourLocked() {
    if (m_state != StreamingPoolState::WaitingForEngine || !m_payload.IsBound()) {
        return false;
    }

    uint64_t observed = m_payload.LoadLock(0);
    while (observed != 0) {
        if (IsPoolSizeWithinLimits(observed, m_policy.limits)) {
            AdoptDetourLocked(observed);
            return true;
        }

        uint64_t expected = observed;
        if (m_payload.CompareExchangeLock(expected, 0)) {
            return false;
        }
        observed = expected;
    }
    return false;
}

bool StreamingPoolController::TryAcquireCVarLockLocked(uint64_t bytes, int32_t engineMb) {
    if (m_state != StreamingPoolState::WaitingForEngine) {
        return false;
    }
    if (!m_payload.IsBound()) {
        PublishLockLocked(bytes, StreamingPoolState::LockedFromCVar, engineMb);
        return true;
    }

    uint64_t observed = 0;
    for (;;) {
        if (m_payload.CompareExchangeLock(observed, bytes)) {
            PublishLockLocked(bytes, StreamingPoolState::LockedFromCVar, engineMb);
            return true;
        }
        if (observed != 0 && IsPoolSizeWithinLimits(observed, m_policy.limits)) {
            AdoptDetourLocked(observed);
            return false;
        }

        uint64_t expected = observed;
        if (observed != 0 && m_payload.CompareExchangeLock(expected, 0)) {
            observed = 0;
            continue;
        }
        observed = expected;
    }
}

void StreamingPoolController::RecordRejectionLocked(
    int32_t poolSizeMb, EnginePoolCandidateValidity reason) {
    m_lastRejectedCandidate = RejectedEnginePoolCandidate{
        .sizeMb = poolSizeMb,
        .reason = reason,
    };
}

void StreamingPoolController::BindPayload(jst::core::StreamingPoolPayload& payload) {
    std::lock_guard lock(m_mutex);
    m_payload.Bind(payload);

    if (m_state == StreamingPoolState::WaitingForEngine ||
        m_state == StreamingPoolState::Unconfigured) {
        const auto priorState = m_state;
        const auto result = ReconcileOpenAutoPolicyLocked();
        if (priorState == StreamingPoolState::Unconfigured) {
            // Preserve the lifecycle state. ArmAuto will adopt any valid
            // orphan; ArmManual will overwrite it.
            if (result != AutoReconcileResult::Fallback) {
                m_state = StreamingPoolState::Unconfigured;
                m_lockedBytes = 0;
                m_effectiveGb = m_policy.limits.FallbackGb();
                m_enginePoolMb = 0;
            }
        }
        return;
    }

    PublishPolicyLocked();
    if (m_lockedBytes != 0) {
        m_payload.StoreLock(m_lockedBytes);
    }
}

bool StreamingPoolController::UpdatePolicy(PoolSizePolicy policy) {
    std::lock_guard lock(m_mutex);
    if (m_policy == policy) {
        return false;
    }
    m_policy = std::move(policy);

    switch (m_state) {
    case StreamingPoolState::Unconfigured:
        m_effectiveGb = m_policy.limits.FallbackGb();
        (void)ReconcileOpenAutoPolicyLocked();
        break;
    case StreamingPoolState::Manual:
        m_effectiveGb = NormalizePoolSizeGb(m_requestedManualGb, m_policy.limits);
        m_lockedBytes = PoolSizeGbToBytes(m_effectiveGb, m_policy.limits);
        m_payload.StoreLock(m_lockedBytes);
        PublishPolicyLocked();
        break;
    case StreamingPoolState::WaitingForEngine:
        m_effectiveGb = m_policy.limits.FallbackGb();
        m_lastRejectedCandidate.reset();
        (void)ReconcileOpenAutoPolicyLocked();
        break;
    case StreamingPoolState::LockedFromCVar:
    case StreamingPoolState::LockedFromDetour:
        if (IsPoolSizeWithinLimits(m_lockedBytes, m_policy.limits)) {
            PublishPolicyLocked();
        } else {
            PublishFallbackLocked();
            PublishPolicyLocked();
        }
        break;
    case StreamingPoolState::Fallback:
        PublishFallbackLocked();
        PublishPolicyLocked();
        break;
    }
    return true;
}

void StreamingPoolController::ArmManual(float requestedPoolSizeGb) {
    std::lock_guard lock(m_mutex);
    m_requestedManualGb = std::isfinite(requestedPoolSizeGb)
        ? requestedPoolSizeGb
        : m_policy.limits.FallbackGb();
    m_effectiveGb = NormalizePoolSizeGb(m_requestedManualGb, m_policy.limits);
    PublishPolicyLocked();
    PublishLockLocked(PoolSizeGbToBytes(m_effectiveGb, m_policy.limits),
                      StreamingPoolState::Manual);
}

void StreamingPoolController::ArmAuto() {
    std::lock_guard lock(m_mutex);

    if (m_payload.IsBound()) {
        const uint64_t orphan = m_payload.LoadLock(0);
        if (orphan != 0 && IsPoolSizeWithinLimits(orphan, m_policy.limits) &&
            m_state == StreamingPoolState::Unconfigured) {
            AdoptDetourLocked(orphan);
            return;
        }
    }
    EnterWaitingLocked();
}

bool StreamingPoolController::UpdateManualSize(float requestedPoolSizeGb) {
    std::lock_guard lock(m_mutex);
    m_requestedManualGb = std::isfinite(requestedPoolSizeGb)
        ? requestedPoolSizeGb
        : m_policy.limits.FallbackGb();
    if (m_state != StreamingPoolState::Manual) {
        return false;
    }
    m_effectiveGb = NormalizePoolSizeGb(m_requestedManualGb, m_policy.limits);
    PublishLockLocked(PoolSizeGbToBytes(m_effectiveGb, m_policy.limits),
                      StreamingPoolState::Manual);
    return true;
}

EnginePoolObservation StreamingPoolController::ObserveEnginePoolMb(int32_t poolSizeMb) {
    std::lock_guard lock(m_mutex);
    if (m_state != StreamingPoolState::WaitingForEngine) {
        return m_state == StreamingPoolState::LockedFromDetour
            ? EnginePoolObservation::LockedFromDetour
            : EnginePoolObservation::Inactive;
    }

    const auto validity = ValidateEnginePoolMb(poolSizeMb, m_policy.limits);
    switch (validity) {
    case EnginePoolCandidateValidity::NotReady:
        return EnginePoolObservation::NotReady;
    case EnginePoolCandidateValidity::BelowMinimum:
        RecordRejectionLocked(poolSizeMb, validity);
        return EnginePoolObservation::RejectedBelowMinimum;
    case EnginePoolCandidateValidity::AboveMaximum:
        RecordRejectionLocked(poolSizeMb, validity);
        return EnginePoolObservation::RejectedAboveMaximum;
    case EnginePoolCandidateValidity::Valid:
        break;
    }

    const auto bytes = EnginePoolMbToBytes(poolSizeMb);
    if (!bytes) {
        return EnginePoolObservation::NotReady;
    }
    return TryAcquireCVarLockLocked(*bytes, poolSizeMb)
        ? EnginePoolObservation::LockedFromCVar
        : EnginePoolObservation::LockedFromDetour;
}

void StreamingPoolController::OnAutoTimeout() {
    std::lock_guard lock(m_mutex);
    if (m_state != StreamingPoolState::WaitingForEngine) {
        return;
    }
    if (TryAdoptLiveDetourLocked()) {
        return;
    }

    const uint64_t fallback = FallbackBytesLocked();
    if (!m_payload.IsBound()) {
        PublishFallbackLocked();
        return;
    }

    uint64_t observed = 0;
    for (;;) {
        if (m_payload.CompareExchangeLock(observed, fallback)) {
            PublishFallbackLocked();
            return;
        }
        if (observed != 0 && IsPoolSizeWithinLimits(observed, m_policy.limits)) {
            AdoptDetourLocked(observed);
            return;
        }
        uint64_t expected = observed;
        if (observed != 0 && m_payload.CompareExchangeLock(expected, fallback)) {
            PublishFallbackLocked();
            return;
        }
        observed = expected;
    }
}

bool StreamingPoolController::PullDetourIfWaiting() {
    std::lock_guard lock(m_mutex);
    return TryAdoptLiveDetourLocked();
}

StreamingPoolSnapshot StreamingPoolController::Snapshot() const {
    std::lock_guard lock(m_mutex);
    return StreamingPoolSnapshot{
        .state = m_state,
        .lockedBytes = m_lockedBytes,
        .effectiveGb = m_effectiveGb,
        .requestedManualGb = m_requestedManualGb,
        .enginePoolMb = m_enginePoolMb,
        .lastRejectedCandidate = m_lastRejectedCandidate,
        .policy = m_policy,
    };
}

bool StreamingPoolController::IsWaitingForEngine() const {
    std::lock_guard lock(m_mutex);
    return m_state == StreamingPoolState::WaitingForEngine;
}

} // namespace jst::tweaks
