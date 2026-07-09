#pragma once

#include "hook_tweak.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace jst::tweaks {

namespace detail {

[[nodiscard]] inline uint64_t GbToPayloadBytes(float gb,
                                               float maxGb,
                                               float defaultGb = 2.0f) {
    constexpr double kBytesPerGB = 1024.0 * 1024.0 * 1024.0;

    const float sanitized = std::isfinite(gb) ? gb : defaultGb;
    const double rawBytes = static_cast<double>(sanitized) * kBytesPerGB;
    const int64_t maxBytes = static_cast<int64_t>(maxGb * kBytesPerGB);
    return static_cast<uint64_t>(
        std::clamp(std::llround(rawBytes), 0LL, maxBytes));
}

} // namespace detail

class StreamingPoolFix final : public HookTweak {
public:
    StreamingPoolFix();

protected:
    void OnRuntimeFloatChanged(float value) override;
};

} // namespace jst::tweaks