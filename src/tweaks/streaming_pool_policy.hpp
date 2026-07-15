#pragma once

#include "core/streaming_pool_protocol.hpp"
#include "slider_utils.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace jst::tweaks {

inline constexpr float kPoolSizeDefaultFallbackGb = 2.0f;
inline constexpr float kPoolSizeSliderStepGb = 0.1f;
inline constexpr uint64_t kPoolSizeVramPercent = 70;

inline constexpr std::string_view kPoolSizeAutoLiteral = "auto";
inline constexpr std::string_view kPoolSizeGbConfigKey = "PoolSizeGB";

enum class PoolSizeMode : uint8_t {
    Auto,
    Manual,
};

struct PoolSizeSetting {
    PoolSizeMode mode = PoolSizeMode::Auto;
    // Retained verbatim across auto/manual toggles. The active policy owns
    // normalization so a later, larger GPU limit can restore the request.
    float requestedManualGb = kPoolSizeDefaultFallbackGb;

    [[nodiscard]] bool IsAuto() const noexcept { return mode == PoolSizeMode::Auto; }
};

struct PoolSizeLimits {
    uint64_t minimumBytes = jst::core::kStreamingPoolMinimumBytes;
    uint64_t maximumBytes = jst::core::kStreamingPoolLegacyCeilingBytes;
    uint64_t fallbackBytes = jst::core::kStreamingPoolDefaultFallbackBytes;

    [[nodiscard]] float MinimumGb() const noexcept;
    [[nodiscard]] float MaximumGb() const noexcept;
    [[nodiscard]] float FallbackGb() const noexcept;

    bool operator==(const PoolSizeLimits&) const = default;
};

struct PoolSizePolicy {
    PoolSizeLimits limits{};
    std::optional<uint64_t> dedicatedVideoMemoryBytes;

    [[nodiscard]] bool HasDedicatedVideoMemory() const noexcept {
        return dedicatedVideoMemoryBytes.has_value();
    }

    bool operator==(const PoolSizePolicy&) const = default;
};

enum class EnginePoolCandidateValidity : uint8_t {
    NotReady,
    BelowMinimum,
    AboveMaximum,
    Valid,
};

inline constexpr PoolSizeLimits kLegacyPoolSizeLimits{};

[[nodiscard]] FloatSliderSpec MakePoolSizeSliderSpec(const PoolSizeLimits& limits) noexcept;
[[nodiscard]] float NormalizePoolSizeGb(
    float gb, const PoolSizeLimits& limits = kLegacyPoolSizeLimits) noexcept;
[[nodiscard]] PoolSizePolicy MakePoolSizePolicy(
    std::optional<uint64_t> dedicatedVideoMemoryBytes) noexcept;

[[nodiscard]] uint64_t PoolSizeGbToBytes(
    float gb, const PoolSizeLimits& limits = kLegacyPoolSizeLimits) noexcept;
[[nodiscard]] std::optional<uint64_t> EnginePoolMbToBytes(int32_t poolSizeMb) noexcept;
[[nodiscard]] float PoolSizeBytesToGb(uint64_t bytes) noexcept;
[[nodiscard]] EnginePoolCandidateValidity ValidateEnginePoolMb(
    int32_t poolSizeMb, const PoolSizeLimits& limits = kLegacyPoolSizeLimits) noexcept;
[[nodiscard]] bool IsPoolSizeWithinLimits(
    uint64_t bytes, const PoolSizeLimits& limits = kLegacyPoolSizeLimits) noexcept;

[[nodiscard]] bool IsPoolSizeAutoLiteral(std::string_view raw) noexcept;
[[nodiscard]] PoolSizeSetting ParsePoolSizeGb(std::string_view raw) noexcept;
[[nodiscard]] std::string FormatPoolSizeGb(const PoolSizeSetting& setting);

} // namespace jst::tweaks
