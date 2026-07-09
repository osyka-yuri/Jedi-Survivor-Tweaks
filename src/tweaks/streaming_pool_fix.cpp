#include "streaming_pool_fix.hpp"
#include "hooks/tweak_hooks.hpp"
#include "slider_specs.hpp"

namespace {

constexpr const char* kPattern =
    "40 E8 ?? ?? ?? ?? 48 8B 54 24 40 84 C0 74 16 48 8B 83 08 01 00 00 48 2B 44 24 48 48 03 C2 48 89";
constexpr int32_t kOffset = 0x6;

} // namespace

namespace jst::tweaks {

StreamingPoolFix::StreamingPoolFix()
    : HookTweak("StreamingPoolFix",
                "Locks the streaming pool size to a configured number of gigabytes (GB) to prevent VRAM memory leaks.",
                false,
                HookTarget::Pattern(kPattern, kOffset, 16),
                reinterpret_cast<std::uintptr_t>(&StreamingPoolFix_Detour),
                jst::hooks::Slot::StreamingPoolFix,
                RuntimeFloatConfig{
                    .slider = kPoolSizeSliderSpec,
                    .configKey = "PoolSizeGB",
                    .sliderLabel = "Pool Size (GB)",
                    .sliderTooltip =
                        "Streaming pool size in GB. Locks the pool to prevent unbounded VRAM "
                        "growth. Takes effect immediately.",
                    .writesToMultiplier = false,
                }) {}

void StreamingPoolFix::OnRuntimeFloatChanged(float value) {
    SetPayload0(detail::GbToPayloadBytes(
        value, kPoolSizeSliderSpec.max, kPoolSizeSliderSpec.defaultValue));
}

} // namespace jst::tweaks