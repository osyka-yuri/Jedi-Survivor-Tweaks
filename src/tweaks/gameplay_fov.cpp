#include "gameplay_fov.hpp"
#include "hooks/tweak_hooks.hpp"

namespace {
    constexpr const char* kPattern =
        "F3 0F ?? ?? 0F ?? ?? 72 ?? 0F ?? ?? F3 0F ?? ?? ?? ?? ?? ?? 0F ?? ?? 0F ?? ?? ?? ?? ?? ?? 0F ?? ?? ?? ?? ?? ?? 76 ??";
    constexpr int32_t kOffset = 0x9;
} // namespace

namespace jst::tweaks {

GameplayFOV::GameplayFOV()
    : HookTweak("GameplayFOV",
                "Adjusts gameplay FOV with a configurable multiplier.",
                false,
                HookTarget::Pattern(kPattern, kOffset),
                reinterpret_cast<std::uintptr_t>(&GameplayFOV_Detour),
                jst::hooks::Slot::GameplayFOV,
                RuntimeFloatConfig{}) {}

} // namespace jst::tweaks
