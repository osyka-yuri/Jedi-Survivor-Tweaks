#pragma once

#include "core/streaming_pool_protocol.hpp"
#include "streaming_pool_policy.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace jst::tweaks {

enum class StreamingPoolState : uint8_t {
    Unconfigured,
    Manual,
    WaitingForEngine,
    LockedFromCVar,
    LockedFromPathSample,
    Fallback,
};

enum class EnginePoolObservation : uint8_t {
    NotReady,
    RejectedBelowMinimum,
    RejectedAboveMaximum,
    LockedFromCVar,
    LockedFromPathSample,
    Inactive,
};

struct RejectedEnginePoolCandidate {
    int32_t sizeMb = 0;
    EnginePoolCandidateValidity reason = EnginePoolCandidateValidity::NotReady;

    bool operator==(const RejectedEnginePoolCandidate&) const = default;
};

struct StreamingPoolSnapshot {
    StreamingPoolState state = StreamingPoolState::Unconfigured;
    uint64_t lockedBytes = 0;
    float effectiveGb = kPoolSizeDefaultFallbackGb;
    float requestedManualGb = kPoolSizeDefaultFallbackGb;
    int32_t enginePoolMb = 0;
    std::optional<RejectedEnginePoolCandidate> lastRejectedCandidate;
    PoolSizePolicy policy{};
};

[[nodiscard]] std::string FormatStreamingPoolStatus(const StreamingPoolSnapshot& snapshot);

class StreamingPoolController final {
public:
    explicit StreamingPoolController(PoolSizePolicy policy = {});

    void BindPayload(jst::core::StreamingPoolPayload& payload);
    [[nodiscard]] bool UpdatePolicy(PoolSizePolicy policy);

    void ArmManual(float requestedPoolSizeGb);
    void ArmAuto();
    [[nodiscard]] bool UpdateManualSize(float requestedPoolSizeGb);

    [[nodiscard]] EnginePoolObservation ObserveEnginePoolMb(int32_t poolSizeMb);
    void OnAutoTimeout();
    // Secondary Auto source after an unusable CVar tick or timeout.
    [[nodiscard]] bool TryAdoptPathSample();

    [[nodiscard]] StreamingPoolSnapshot Snapshot() const;
    [[nodiscard]] bool IsWaitingForEngine() const;

private:
    class PayloadPort final {
    public:
        void Bind(jst::core::StreamingPoolPayload& payload) noexcept;
        void StoreForced(uint64_t value) const noexcept;
        void PublishPolicy(uint64_t ceiling, uint64_t fallback) const noexcept;
        [[nodiscard]] uint64_t LoadFirstObserved() const noexcept;

    private:
        jst::core::StreamingPoolPayload* m_payload = nullptr;
    };

    // Caller holds m_mutex for every method suffixed Locked.
    void PublishPolicyLocked() const noexcept;
    void EnterAutoWaitingLocked();
    void PublishSafeHoldLocked();
    void PublishLockLocked(uint64_t bytes, StreamingPoolState state, int32_t engineMb = 0);
    void PublishFallbackLocked();
    [[nodiscard]] bool TryAdoptPathSampleLocked();
    void RecordRejectionLocked(int32_t poolSizeMb, EnginePoolCandidateValidity reason);

    PoolSizePolicy m_policy;
    PayloadPort m_payload;
    mutable std::mutex m_mutex;
    StreamingPoolState m_state = StreamingPoolState::Unconfigured;
    uint64_t m_lockedBytes = 0;
    float m_effectiveGb = kPoolSizeDefaultFallbackGb;
    float m_requestedManualGb = kPoolSizeDefaultFallbackGb;
    int32_t m_enginePoolMb = 0;
    std::optional<RejectedEnginePoolCandidate> m_lastRejectedCandidate;
};

} // namespace jst::tweaks
