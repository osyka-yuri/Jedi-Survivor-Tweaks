#include "letterbox_pillarbox_fix.hpp"
#include "hooks/tweak_hooks.hpp"

namespace {
    constexpr const char* kPattern = "EB ?? F3 0F ?? ?? ?? ?? ?? ?? F3 0F ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? 8B ?? ?? ?? ?? ??";
    // Hook at the start of the three-instruction sequence (Groups 4-6 of the
    // scanned pattern, offset 0x0F = 15 bytes from the EB opcode):
    //   mov eax, [rbx+02BCh]       ; copy display width to render context
    //   mov [rdi+030h], eax
    //   mov eax, [rbx+02C8h]       ; read aspect-ratio constraint flag
    // Total replaced: 6+3+6 = 15 bytes >= kAbsoluteJmpSize (14).
    constexpr int32_t kOffset = 0x0F;
} // namespace

namespace jst::tweaks {

LetterboxPillarboxFix::LetterboxPillarboxFix()
    : HookTweak("LetterboxPillarboxFix",
                "Removes pillarboxing/letterboxing in cutscenes by disabling aspect ratio constraints.",
                true,
                HookTarget::Pattern(kPattern, kOffset, 15),
                reinterpret_cast<std::uintptr_t>(&LetterboxPillarboxFix_Detour),
                jst::hooks::Slot::LetterboxPillarboxFix) {}

} // namespace jst::tweaks
