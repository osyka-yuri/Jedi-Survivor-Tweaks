#pragma once

#include <cstdint>
#include <type_traits>

namespace jst::core {

inline constexpr uint64_t kBytesPerMiB = 1ull << 20;
inline constexpr uint64_t kBytesPerGiB = 1ull << 30;

inline constexpr uint64_t kStreamingPoolMinimumBytes = kBytesPerGiB / 2;
inline constexpr uint64_t kStreamingPoolLegacyCeilingBytes = 12ull * kBytesPerGiB;
inline constexpr uint64_t kStreamingPoolDefaultFallbackBytes = 2ull * kBytesPerGiB;

// Shared C++/MASM protocol for StreamingPoolFix. Every C++ access to these
// words goes through std::atomic_ref in StreamingPoolController; the x64
// detour uses aligned 64-bit loads and a locked cmpxchg.
struct StreamingPoolPayload {
    uint64_t lockedBytes = 0;
    uint64_t captureCeilingBytes = kStreamingPoolLegacyCeilingBytes;
    uint64_t fallbackBytes = kStreamingPoolDefaultFallbackBytes;
};

static_assert(sizeof(StreamingPoolPayload) == 24);
static_assert(alignof(StreamingPoolPayload) == alignof(uint64_t));
static_assert(std::is_standard_layout_v<StreamingPoolPayload>);
static_assert(std::is_trivially_copyable_v<StreamingPoolPayload>);

} // namespace jst::core
