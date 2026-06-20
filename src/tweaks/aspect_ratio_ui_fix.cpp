#include "aspect_ratio_ui_fix.hpp"
#include "hooks/tweak_hooks.hpp"

namespace {
    // Original instructions at the patch site (14 bytes total):
    //   movaps xmm4, xmm0          ; 0F 28 E0
    //   divss  xmm4, [rbp+1F8h]    ; F3 0F 5E A5 F8 01 00 00
    //   movaps xmm0, xmm4          ; 0F 28 C4
    constexpr const char* kPattern = "0F 28 E0 F3 0F 5E A5 F8 01 00 00 0F 28 C4";
    constexpr int32_t kOffset = 0;

    // The patched instruction holds a UI scale proportional to render height
    // (~height/1440), so a 16:10 display -- 10/9 taller than 16:9 at the same
    // width -- gets a UI 10/9 too large. The detour multiplies that value by
    // Multiplier, so a factor of 0.9 (= 9/10) maps ANY 16:10 resolution onto its
    // 16:9 equivalent (1.1111*0.9=1.0, 0.8333*0.9=0.75) while 1.0 is a no-op,
    // safe on any aspect ratio. Default is the 16:10 fix; the range allows
    // fine-tuning plus modest up/down UI scaling.
    constexpr float kDefaultMultiplier = 0.9f;
    constexpr float kMultiplierMin     = 0.5f;
    constexpr float kMultiplierMax     = 1.5f;
}

namespace jst::tweaks {

AspectRatioUIFix::AspectRatioUIFix()
    : HookTweak("AspectRatioUIFix",
                "Fixes oversized UI on 16:10 displays. The game scales the UI by a height-based factor, so 16:10 (10/9 taller than 16:9) is 10/9 too large. Multiplier MULTIPLIES that factor: 0.9 maps any 16:10 resolution to its 16:9 equivalent (default), 1.0 = no change. Resolution-independent. Range 0.5-1.5.",
                false,
                HookTarget::Pattern(kPattern, kOffset),
                reinterpret_cast<std::uintptr_t>(&AspectRatioUIFix_Detour),
                jst::hooks::Slot::AspectRatioUIFix,
                MultiplierConfig{.defaultValue = kDefaultMultiplier, .clampMin = kMultiplierMin, .clampMax = kMultiplierMax}) {}

} // namespace jst::tweaks
