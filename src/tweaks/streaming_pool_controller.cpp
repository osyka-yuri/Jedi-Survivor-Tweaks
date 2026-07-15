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
        if (snapshot.lockedBytes != 0) {
            status += std::format(" (holding {:.1f} GB)", snapshot.effectiveGb);
        }
        break;
    case StreamingPoolState::LockedFromCVar:
        status = std::format("Locked to engine: {:.2f} GB ({} MB)",
                             snapshot.effectiveGb, snapshot.enginePoolMb);
        break;
    case StreamingPoolState::LockedFromPathSample:
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

void StreamingPoolController::PayloadPort::StoreForced(uint64_t value) const noexcept {
    if (m_payload) {
        std::atomic_ref<uint64_t>(m_payload->forcedBytes)
            .store(value, std::memory_order_release);
    }
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

uint64_t StreamingPoolController::PayloadPort::LoadFirstObserved() const noexcept {
    if (!m_payload) {
        return 0;
    }
    return std::atomic_ref<uint64_t>(m_payload->firstObservedEngineBytes)
        .load(std::memory_order_acquire);
}

void StreamingPoolController::PublishPolicyLocked() const noexcept {
    m_payload.PublishPolicy(
        m_policy.limits.maximumBytes, m_policy.limits.fallbackBytes);
}

void StreamingPoolController::EnterAutoWaitingLocked() {
    m_enginePoolMb = 0;
    m_lastRejectedCandidate.reset();
    m_state = StreamingPoolState::WaitingForEngine;
    PublishSafeHoldLocked();
}

void StreamingPoolController::PublishLockLocked(
    uint64_t bytes, StreamingPoolState state, int32_t engineMb) {
    m_lockedBytes = bytes;
    m_effectiveGb = PoolSizeBytesToGb(bytes);
    m_enginePoolMb = engineMb;
    m_lastRejectedCandidate.reset();
    m_state = state;
    m_payload.StoreForced(bytes);
}

void StreamingPoolController::PublishFallbackLocked() {
    PublishLockLocked(m_policy.limits.fallbackBytes, StreamingPoolState::Fallback);
}

bool StreamingPoolController::TryAdoptPathSampleLocked() {
    const uint64_t observed = m_payload.LoadFirstObserved();
    if (observed == 0 || !IsPoolSizeWithinLimits(observed, m_policy.limits)) {
        return false;
    }
    PublishLockLocked(observed, StreamingPoolState::LockedFromPathSample);
    return true;
}

void StreamingPoolController::PublishSafeHoldLocked() {
    if (m_lockedBytes == 0 || !IsPoolSizeWithinLimits(m_lockedBytes, m_policy.limits)) {
        m_lockedBytes = m_policy.limits.fallbackBytes;
    }
    m_effectiveGb = PoolSizeBytesToGb(m_lockedBytes);
    m_payload.StoreForced(m_lockedBytes);
    PublishPolicyLocked();
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

    if (m_state == StreamingPoolState::Unconfigured) {
        PublishPolicyLocked();
        m_lockedBytes = 0;
        m_effectiveGb = m_policy.limits.FallbackGb();
        m_enginePoolMb = 0;
        m_payload.StoreForced(0);
        return;
    }

    if (m_state == StreamingPoolState::WaitingForEngine) {
        PublishSafeHoldLocked();
        return;
    }

    if (m_lockedBytes != 0) {
        m_payload.StoreForced(m_lockedBytes);
    }
    PublishPolicyLocked();
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
        PublishPolicyLocked();
        break;
    case StreamingPoolState::Manual:
        PublishLockLocked(
            PoolSizeGbToBytes(m_requestedManualGb, m_policy.limits),
            StreamingPoolState::Manual);
        PublishPolicyLocked();
        break;
    case StreamingPoolState::WaitingForEngine:
        m_lastRejectedCandidate.reset();
        PublishSafeHoldLocked();
        break;
    case StreamingPoolState::LockedFromCVar:
    case StreamingPoolState::LockedFromPathSample:
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
    PublishLockLocked(PoolSizeGbToBytes(m_requestedManualGb, m_policy.limits),
                      StreamingPoolState::Manual);
    PublishPolicyLocked();
}

void StreamingPoolController::ArmAuto() {
    std::lock_guard lock(m_mutex);
    EnterAutoWaitingLocked();
}

bool StreamingPoolController::UpdateManualSize(float requestedPoolSizeGb) {
    std::lock_guard lock(m_mutex);
    m_requestedManualGb = std::isfinite(requestedPoolSizeGb)
        ? requestedPoolSizeGb
        : m_policy.limits.FallbackGb();
    if (m_state != StreamingPoolState::Manual) {
        return false;
    }
    PublishLockLocked(PoolSizeGbToBytes(m_requestedManualGb, m_policy.limits),
                      StreamingPoolState::Manual);
    return true;
}

EnginePoolObservation StreamingPoolController::ObserveEnginePoolMb(int32_t poolSizeMb) {
    std::lock_guard lock(m_mutex);
    if (m_state != StreamingPoolState::WaitingForEngine) {
        return m_state == StreamingPoolState::LockedFromPathSample
            ? EnginePoolObservation::LockedFromPathSample
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

    PublishLockLocked(*bytes, StreamingPoolState::LockedFromCVar, poolSizeMb);
    return EnginePoolObservation::LockedFromCVar;
}

void StreamingPoolController::OnAutoTimeout() {
    std::lock_guard lock(m_mutex);
    if (m_state != StreamingPoolState::WaitingForEngine) {
        return;
    }
    if (TryAdoptPathSampleLocked()) {
        return;
    }
    PublishFallbackLocked();
}

bool StreamingPoolController::TryAdoptPathSample() {
    std::lock_guard lock(m_mutex);
    if (m_state != StreamingPoolState::WaitingForEngine) {
        return false;
    }
    return TryAdoptPathSampleLocked();
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
