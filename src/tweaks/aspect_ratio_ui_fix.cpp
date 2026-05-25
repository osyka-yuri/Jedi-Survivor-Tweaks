#include "aspect_ratio_ui_fix.hpp"
#include "hooks/tweak_hooks.hpp"

namespace {
    // Original instructions at the patch site (14 bytes total):
    //   movaps xmm4, xmm0          ; 0F 28 E0
    //   divss  xmm4, [rbp+1F8h]    ; F3 0F 5E A5 F8 01 00 00
    //   movaps xmm0, xmm4          ; 0F 28 C4
    constexpr const char* kPattern = "0F 28 E0 F3 0F 5E A5 F8 01 00 00 0F 28 C4";
    constexpr int32_t kOffset = 0;

    // The game multiplies the UI scale by 1.7777..(16:9) / 1.6 = 10/9 = 1.111..
    // when running on a 16:10 display. The detour replaces that constant with
    // a smaller multiplier so the resulting UI scale is no longer stretched.
    // The clamp upper bound matches the original game constant -- setting
    // Multiplier to it makes the hook a no-op.
    constexpr float kMaxUIScale10Over9 = 10.0f / 9.0f;
}

namespace jst::tweaks {

AspectRatioUIFix::AspectRatioUIFix()
    : HookTweak("AspectRatioUIFix",
                "Fixes UI stretching on 16:10 displays: replaces the 16:10 aspect constant (1.111) in the UI scale formula (value / 1.5) so the result is 0.667 instead of 0.740.",
                false,
                HookTarget::Pattern(kPattern, kOffset),
                reinterpret_cast<std::uintptr_t>(&AspectRatioUIFix_Detour),
                jst::hooks::Slot::AspectRatioUIFix,
                MultiplierConfig{.defaultValue = 1.0f, .clampMin = 1.0f, .clampMax = kMaxUIScale10Over9}) {}

} // namespace jst::tweaks
