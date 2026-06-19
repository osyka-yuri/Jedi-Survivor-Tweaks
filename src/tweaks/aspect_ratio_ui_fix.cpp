#include "aspect_ratio_ui_fix.hpp"
#include "hooks/tweak_hooks.hpp"

namespace {
    // Original instructions at the patch site (14 bytes total):
    //   movaps xmm4, xmm0          ; 0F 28 E0
    //   divss  xmm4, [rbp+1F8h]    ; F3 0F 5E A5 F8 01 00 00
    //   movaps xmm0, xmm4          ; 0F 28 C4
    constexpr const char* kPattern = "0F 28 E0 F3 0F 5E A5 F8 01 00 00 0F 28 C4";
    constexpr int32_t kOffset = 0;

    // On a 16:10 display the game feeds 1.7777..(16:9) / 1.6 = 10/9 = 1.111..
    // into the UI scale formula (value / 1.5), which stretches the UI. While
    // enabled the detour unconditionally forces this numerator to the configured
    // Multiplier (default 1.0 -> 1.0 / 1.5 = 0.667, the un-stretched result).
    // The Multiplier is the absolute numerator now, not a "replace 1.111" guard,
    // so it is clamped to a small band around 1.0 for safe live tuning.
    constexpr float kMultiplierMin = 0.9f;
    constexpr float kMultiplierMax = 1.2f;
}

namespace jst::tweaks {

AspectRatioUIFix::AspectRatioUIFix()
    : HookTweak("AspectRatioUIFix",
                "Fixes UI stretching on 16:10 displays: while enabled, forces the UI scale numerator (value / 1.5) to the configured Multiplier. Default 1.0 gives 0.667 instead of the stretched 0.740. Multiplier range 0.9-1.2.",
                false,
                HookTarget::Pattern(kPattern, kOffset),
                reinterpret_cast<std::uintptr_t>(&AspectRatioUIFix_Detour),
                jst::hooks::Slot::AspectRatioUIFix,
                MultiplierConfig{.defaultValue = 1.0f, .clampMin = kMultiplierMin, .clampMax = kMultiplierMax}) {}

} // namespace jst::tweaks
