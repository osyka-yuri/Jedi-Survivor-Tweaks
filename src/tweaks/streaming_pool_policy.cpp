#include "streaming_pool_policy.hpp"

#include "core/ini_helpers.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace jst::tweaks {

namespace {

[[nodiscard]] uint64_t TenthsOfGiBToBytes(uint64_t tenthSteps) noexcept {
    // A decimal tenth of a binary GiB is fractional. Floor the remainder so
    // a configured/capture ceiling can never exceed the VRAM policy by a
    // handful of bytes due to float representation.
    return (tenthSteps / 10) * kPoolSizeBytesPerGiB +
           (tenthSteps % 10) * kPoolSizeBytesPerGiB / 10;
}

} // namespace

float PoolSizeLimits::MinimumGb() const noexcept {
    return PoolSizeBytesToGb(minimumBytes);
}

float PoolSizeLimits::MaximumGb() const noexcept {
    return PoolSizeBytesToGb(maximumBytes);
}

float PoolSizeLimits::DefaultGb() const noexcept {
    return PoolSizeBytesToGb(defaultBytes);
}

FloatSliderSpec MakePoolSizeSliderSpec(const PoolSizeLimits& limits) noexcept {
    return FloatSliderSpec{
        .min = limits.MinimumGb(),
        .max = limits.MaximumGb(),
        .defaultValue = limits.DefaultGb(),
        .step = kPoolSizeSliderStepGb,
    };
}

float NormalizePoolSizeGb(float gb, const PoolSizeLimits& limits) noexcept {
    const float finite = std::isfinite(gb) ? gb : limits.DefaultGb();
    return LoadSliderValue(finite, MakePoolSizeSliderSpec(limits));
}

PoolSizePolicy MakePoolSizePolicy(
    std::optional<uint64_t> dedicatedVideoMemoryBytes) noexcept {
    if (!dedicatedVideoMemoryBytes || *dedicatedVideoMemoryBytes == 0) {
        return {};
    }

    // floor((70% * bytes) / 0.1 GiB) without overflowing bytes * 10.
    constexpr uint64_t kTenthsPerGiBAtLimit = kPoolSizeVramPercent / 10;
    static_assert(kPoolSizeVramPercent % 10 == 0);
    const uint64_t wholeGiB = *dedicatedVideoMemoryBytes / kPoolSizeBytesPerGiB;
    const uint64_t remainder = *dedicatedVideoMemoryBytes % kPoolSizeBytesPerGiB;
    const uint64_t tenthSteps =
        wholeGiB * kTenthsPerGiBAtLimit +
        remainder * kTenthsPerGiBAtLimit / kPoolSizeBytesPerGiB;

    const uint64_t maximumBytes = std::max(
        kPoolSizeMinimumBytes,
        TenthsOfGiBToBytes(tenthSteps));
    const uint64_t defaultBytes = std::min(
        kPoolSizeManualDefaultBytes, maximumBytes);

    return PoolSizePolicy{
        .limits = PoolSizeLimits{
            .minimumBytes = kPoolSizeMinimumBytes,
            .maximumBytes = maximumBytes,
            .defaultBytes = defaultBytes,
        },
        .dedicatedVideoMemoryBytes = dedicatedVideoMemoryBytes,
    };
}

uint64_t PoolSizeGbToBytes(float gb, const PoolSizeLimits& limits) noexcept {
    const float normalized = NormalizePoolSizeGb(gb, limits);
    const auto tenthSteps = static_cast<uint64_t>(std::llround(
        static_cast<double>(normalized) * 10.0));
    return std::clamp(
        TenthsOfGiBToBytes(tenthSteps), limits.minimumBytes, limits.maximumBytes);
}

std::optional<uint64_t> EnginePoolMbToBytes(int32_t poolSizeMb) noexcept {
    if (poolSizeMb <= 0) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(poolSizeMb) * kPoolSizeBytesPerMiB;
}

float PoolSizeBytesToGb(uint64_t bytes) noexcept {
    return static_cast<float>(
        static_cast<double>(bytes) / static_cast<double>(kPoolSizeBytesPerGiB));
}

bool IsPoolSizeAutoLiteral(std::string_view raw) noexcept {
    return jst::core::detail::EqualIgnoreCase(
        jst::core::detail::Trim(raw), kPoolSizeAutoLiteral);
}

PoolSizeSetting ParsePoolSizeGb(std::string_view raw) noexcept {
    if (IsPoolSizeAutoLiteral(raw)) {
        return {};
    }

    const auto trimmed = jst::core::detail::Trim(raw);
    if (trimmed.empty()) {
        return {};
    }

    float value = 0.0f;
    const char* begin = trimmed.data();
    const char* end = begin + trimmed.size();
    const auto [ptr, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || ptr != end || !std::isfinite(value)) {
        return {};
    }

    return PoolSizeSetting{
        .mode = PoolSizeMode::Manual,
        .requestedManualGb = value,
    };
}

std::string FormatPoolSizeGb(const PoolSizeSetting& setting) {
    if (setting.IsAuto()) {
        return std::string(kPoolSizeAutoLiteral);
    }
    const float finite = std::isfinite(setting.requestedManualGb)
        ? setting.requestedManualGb
        : kPoolSizeDefaultGb;
    return std::format("{:.1f}", finite);
}

} // namespace jst::tweaks
