#include "streaming_pool_controller.hpp"

#include <atomic>
#include <cmath>
#include <format>
#include <utility>

namespace jst::tweaks {

namespace {

// This is deliberately not the manual-default authority. It is the only
// automatic terminal selection for an unavailable, invalid, or nonpositive
// engine read and is never persisted or subject to the manual GPU policy.
inline constexpr uint64_t kAutomaticFallbackBytes =
    2ull * kPoolSizeBytesPerGiB;

} // namespace

std::string FormatStreamingPoolStatus(const StreamingPoolSnapshot& snapshot) {
    std::string status;
    switch (snapshot.state) {
    case StreamingPoolState::Unconfigured:
        status = "Not configured";
        break;
    case StreamingPoolState::WaitingForEngine:
        status = snapshot.lockedBytes == 0
            ? "Waiting for engine pool size..."
            : std::format("Waiting for engine pool size... holding {:.1f} GB",
                          snapshot.effectiveGb);
        break;
    case StreamingPoolState::Automatic:
        status = std::format("Auto: {:.2f} GB (engine {} MB)",
                             snapshot.effectiveGb, snapshot.enginePoolMb);
        break;
    case StreamingPoolState::AutomaticFallback:
        status = "Auto fallback: 2.00 GB";
        break;
    case StreamingPoolState::Manual:
        status = std::format("Manual: {:.1f} GB", snapshot.effectiveGb);
        if (!std::isfinite(snapshot.requestedManualGb) ||
            snapshot.requestedManualGb != snapshot.effectiveGb) {
            status += std::format(" (requested {:.1f} GB)",
                                  snapshot.requestedManualGb);
        }
        break;
    }
    if (snapshot.policy.dedicatedVideoMemoryBytes) {
        status += std::format(" | GPU {:.1f} GB, manual max {:.1f} GB",
                              PoolSizeBytesToGb(*snapshot.policy.dedicatedVideoMemoryBytes),
                              snapshot.policy.limits.MaximumGb());
    } else {
        status += " | VRAM unavailable; manual max 12.0 GB";
    }
    return status;
}

StreamingPoolController::StreamingPoolController(PoolSizePolicy policy)
    : m_policy(std::move(policy)),
      m_effectiveGb(m_policy.limits.DefaultGb()),
      m_requestedManualGb(m_policy.limits.DefaultGb()) {}

void StreamingPoolController::PublishLocked(uint64_t bytes) const noexcept {
    if (m_payload) {
        std::atomic_ref<uint64_t>(m_payload->forcedBytes)
            .store(bytes, std::memory_order_release);
    }
}

void StreamingPoolController::BindPayload(
    jst::core::StreamingPoolPayload& payload) {
    std::lock_guard lock(m_mutex);
    m_payload = &payload;
    PublishLocked(m_lockedBytes);
}

bool StreamingPoolController::UpdatePolicy(PoolSizePolicy policy) {
    std::lock_guard lock(m_mutex);
    if (m_policy == policy) {
        return false;
    }
    m_policy = std::move(policy);
    if (m_state == StreamingPoolState::Manual) {
        PublishManualLocked();
    }
    return true;
}

void StreamingPoolController::ArmAuto(uint64_t generation) {
    std::lock_guard lock(m_mutex);
    m_generation = generation;
    m_state = StreamingPoolState::WaitingForEngine;
    m_enginePoolMb = 0;
    // Manual -> Auto retains the old forced value only while this generation
    // waits. Auto terminal values always replace it exactly once.
    PublishLocked(m_lockedBytes);
}

StreamingPoolAutoReadResult StreamingPoolController::CompleteAutoRead(
    uint64_t generation,
    std::expected<int32_t, std::string> enginePoolMb) {
    std::lock_guard lock(m_mutex);
    if (generation != m_generation ||
        m_state != StreamingPoolState::WaitingForEngine) {
        return {.completion = StreamingPoolAutoCompletion::Stale,
                .snapshot = {.state = m_state,
                             .lockedBytes = m_lockedBytes,
                             .effectiveGb = m_effectiveGb,
                             .requestedManualGb = m_requestedManualGb,
                             .enginePoolMb = m_enginePoolMb,
                             .generation = m_generation,
                             .policy = m_policy}};
    }

    if (enginePoolMb) {
        if (const auto bytes = EnginePoolMbToBytes(*enginePoolMb)) {
            m_state = StreamingPoolState::Automatic;
            m_enginePoolMb = *enginePoolMb;
            m_lockedBytes = *bytes;
            m_effectiveGb = PoolSizeBytesToGb(*bytes);
            PublishLocked(*bytes);
            return {.completion = StreamingPoolAutoCompletion::Exact,
                    .snapshot = {.state = m_state,
                                 .lockedBytes = m_lockedBytes,
                                 .effectiveGb = m_effectiveGb,
                                 .requestedManualGb = m_requestedManualGb,
                                 .enginePoolMb = m_enginePoolMb,
                                 .generation = m_generation,
                                 .policy = m_policy}};
        }
    }

    const std::string diagnostic = enginePoolMb
        ? "engine pool size must be a positive MB value"
        : enginePoolMb.error();
    m_state = StreamingPoolState::AutomaticFallback;
    m_enginePoolMb = 0;
    m_lockedBytes = kAutomaticFallbackBytes;
    m_effectiveGb = PoolSizeBytesToGb(kAutomaticFallbackBytes);
    PublishLocked(m_lockedBytes);
    return {.completion = StreamingPoolAutoCompletion::Fallback,
            .snapshot = {.state = m_state,
                         .lockedBytes = m_lockedBytes,
                         .effectiveGb = m_effectiveGb,
                         .requestedManualGb = m_requestedManualGb,
                         .enginePoolMb = 0,
                         .generation = m_generation,
                         .policy = m_policy},
            .diagnostic = diagnostic};
}

void StreamingPoolController::PublishManualLocked() {
    m_lockedBytes = PoolSizeGbToBytes(m_requestedManualGb, m_policy.limits);
    m_effectiveGb = PoolSizeBytesToGb(m_lockedBytes);
    m_enginePoolMb = 0;
    PublishLocked(m_lockedBytes);
}

void StreamingPoolController::ArmManual(
    uint64_t generation,
    float requestedPoolSizeGb) {
    std::lock_guard lock(m_mutex);
    m_generation = generation;
    m_requestedManualGb = std::isfinite(requestedPoolSizeGb)
        ? requestedPoolSizeGb
        : m_policy.limits.DefaultGb();
    m_state = StreamingPoolState::Manual;
    PublishManualLocked();
}

StreamingPoolSnapshot StreamingPoolController::Snapshot() const {
    std::lock_guard lock(m_mutex);
    return {.state = m_state,
            .lockedBytes = m_lockedBytes,
            .effectiveGb = m_effectiveGb,
            .requestedManualGb = m_requestedManualGb,
            .enginePoolMb = m_enginePoolMb,
            .generation = m_generation,
            .policy = m_policy};
}

} // namespace jst::tweaks
