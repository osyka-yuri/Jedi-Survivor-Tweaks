#pragma once

#include "core/streaming_pool_protocol.hpp"
#include "streaming_pool_policy.hpp"

#include <cstdint>
#include <expected>
#include <mutex>
#include <string>

namespace jst::tweaks {

enum class StreamingPoolState : uint8_t {
    Unconfigured,
    WaitingForEngine,
    Automatic,
    AutomaticFallback,
    Manual,
};

enum class StreamingPoolAutoCompletion : uint8_t {
    Exact,
    Fallback,
    Stale,
};

struct StreamingPoolSnapshot {
    StreamingPoolState state = StreamingPoolState::Unconfigured;
    uint64_t lockedBytes = 0;
    float effectiveGb = kPoolSizeDefaultGb;
    float requestedManualGb = kPoolSizeDefaultGb;
    int32_t enginePoolMb = 0;
    uint64_t generation = 0;
    PoolSizePolicy policy{};
};

struct StreamingPoolAutoReadResult {
    StreamingPoolAutoCompletion completion = StreamingPoolAutoCompletion::Stale;
    StreamingPoolSnapshot snapshot{};
    std::string diagnostic;
};

[[nodiscard]] std::string FormatStreamingPoolStatus(
    const StreamingPoolSnapshot& snapshot);

class StreamingPoolController final {
public:
    explicit StreamingPoolController(PoolSizePolicy policy = {});

    void BindPayload(jst::core::StreamingPoolPayload& payload);
    [[nodiscard]] bool UpdatePolicy(PoolSizePolicy policy);

    void ArmAuto(uint64_t generation);
    [[nodiscard]] StreamingPoolAutoReadResult CompleteAutoRead(
        uint64_t generation,
        std::expected<int32_t, std::string> enginePoolMb);
    void ArmManual(uint64_t generation, float requestedPoolSizeGb);

    [[nodiscard]] StreamingPoolSnapshot Snapshot() const;

private:
    void PublishLocked(uint64_t bytes) const noexcept;
    void PublishManualLocked();

    PoolSizePolicy m_policy;
    jst::core::StreamingPoolPayload* m_payload = nullptr;
    mutable std::mutex m_mutex;
    StreamingPoolState m_state = StreamingPoolState::Unconfigured;
    uint64_t m_lockedBytes = 0;
    float m_effectiveGb = kPoolSizeDefaultGb;
    float m_requestedManualGb = kPoolSizeDefaultGb;
    int32_t m_enginePoolMb = 0;
    uint64_t m_generation = 0;
};

} // namespace jst::tweaks
