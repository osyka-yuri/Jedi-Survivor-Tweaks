#pragma once

#include <cstdint>
#include <type_traits>

namespace jst::core {

inline constexpr uint64_t kBytesPerMiB = 1ull << 20;
inline constexpr uint64_t kBytesPerGiB = 1ull << 30;

inline constexpr uint64_t kStreamingPoolMinimumBytes = kBytesPerGiB / 2;
inline constexpr uint64_t kStreamingPoolLegacyCeilingBytes = 12ull * kBytesPerGiB;
inline constexpr uint64_t kStreamingPoolDefaultFallbackBytes = 2ull * kBytesPerGiB;

// Shared C++/MASM protocol for StreamingPoolFix. C++ owns forcedBytes and the
// policy words. The detour reads those words and publishes the first in-range
// engine sample with a one-shot compare-exchange. Every C++ access is atomic.
struct StreamingPoolPayload {
    uint64_t forcedBytes = 0;
    uint64_t captureCeilingBytes = kStreamingPoolLegacyCeilingBytes;
    uint64_t fallbackBytes = kStreamingPoolDefaultFallbackBytes;
    uint64_t firstObservedEngineBytes = 0;
};

static_assert(sizeof(StreamingPoolPayload) == 32);
static_assert(alignof(StreamingPoolPayload) == alignof(uint64_t));
static_assert(std::is_standard_layout_v<StreamingPoolPayload>);
static_assert(std::is_trivially_copyable_v<StreamingPoolPayload>);

} // namespace jst::core
