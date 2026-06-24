#include "streaming_pool_fix.hpp"
#include "hooks/tweak_hooks.hpp"

#include <algorithm>
#include <cmath>

namespace {
    // Byte-pattern for the streaming-pool size calculation. kOffset skips the
    // 6-byte `40 E8 ?? ?? ?? ??` (REX + CALL rel32) and lands on the 16-byte
    // window the detour replaces:
    //   mov rdx, [rsp+40h]   (5)  -- pool size (overridden by payload0)
    //   test al, al          (2)  -- flags from the CALL above
    //   jz +0x16             (2)  -- skip recalc path
    //   mov rax, [rbx+0108h] (7)  -- recalc path
    constexpr const char* kPattern = "40 E8 ?? ?? ?? ?? 48 8B 54 24 40 84 C0 74 16 48 8B 83 08 01 00 00 48 2B 44 24 48 48 03 C2 48 89";
    constexpr int32_t kOffset = 0x6;

    constexpr float kDefaultPoolSizeGB = 2.0f;
    constexpr float kMinPoolSizeGB     = 1.0f;
    constexpr float kMaxPoolSizeGB     = 12.0f;
} // namespace

namespace jst::tweaks {

StreamingPoolFix::StreamingPoolFix()
    : HookTweak("StreamingPoolFix",
                "Locks the streaming pool size to a configured number of gigabytes (GB) to prevent VRAM memory leaks.",
                false,
                HookTarget::Pattern(kPattern, kOffset),
                reinterpret_cast<std::uintptr_t>(&StreamingPoolFix_Detour),
                jst::hooks::Slot::StreamingPoolFix,
                MultiplierConfig{
                    .configKey     = "PoolSizeGB",
                    .defaultValue  = kDefaultPoolSizeGB,
                    .clampMin      = kMinPoolSizeGB,
                    .clampMax      = kMaxPoolSizeGB,
                    .sliderLabel   = "Pool Size (GB)",
                    .sliderTooltip = "Streaming pool size in GB. Locks the pool to prevent unbounded VRAM "
                                     "growth. Takes effect immediately.",
                }) {}

void StreamingPoolFix::OnMultiplierChanged(float value) {
    constexpr double kBytesPerGB = 1024.0 * 1024.0 * 1024.0;

    // Guard against NaN/Inf slipping in from a malformed config or slider.
    // The clamp path already bounds finite values to [1, 24]; this only
    // short-circuits the non-finite case to the default.
    const float gb = std::isfinite(value) ? value : kDefaultPoolSizeGB;

    // Multiply in double to preserve sub-integer precision (e.g. 1.5 GB)
    // before rounding to bytes. payload0 is read directly by the detour, so
    // no runtime arithmetic happens on the hot path.
    const double rawBytes = static_cast<double>(gb) * kBytesPerGB;
    const int64_t maxBytes = static_cast<int64_t>(kMaxPoolSizeGB * kBytesPerGB);
    const uint64_t poolBytes = static_cast<uint64_t>(
        std::clamp(std::llround(rawBytes), 0LL, maxBytes));

    SetPayload0(poolBytes);
}

} // namespace jst::tweaks
