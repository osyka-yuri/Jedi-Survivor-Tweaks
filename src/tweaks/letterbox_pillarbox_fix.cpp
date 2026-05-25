#include "letterbox_pillarbox_fix.hpp"
#include "hooks/tweak_hooks.hpp"

namespace {
    constexpr const char* kPattern = "EB ?? F3 0F ?? ?? ?? ?? ?? ?? F3 0F ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? 8B ?? ?? ?? ?? ??";
    constexpr int32_t kOffset = 0x18;
}

namespace jst::tweaks {

LetterboxPillarboxFix::LetterboxPillarboxFix()
    : HookTweak("LetterboxPillarboxFix",
                "Removes pillarboxing/letterboxing in cutscenes by disabling aspect ratio constraints.",
                true,
                HookTarget::Pattern(kPattern, kOffset),
                reinterpret_cast<std::uintptr_t>(&LetterboxPillarboxFix_Detour),
                jst::hooks::Slot::LetterboxPillarboxFix) {}

} // namespace jst::tweaks
