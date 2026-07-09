#include "camera_distance.hpp"
#include "hooks/tweak_hooks.hpp"

namespace {
    constexpr const char* kPattern =
        "0F ?? ?? 72 ?? 0F ?? ?? F3 0F ?? ?? ?? ?? ?? ?? F3 0F ?? ?? ?? ?? ?? ?? 0F ?? ?? F3 0F ?? ?? 0F ";
    constexpr int32_t kOffset = 0x8;
} // namespace

namespace jst::tweaks {

CameraDistance::CameraDistance()
    : HookTweak("CameraDistance",
                "Adjusts gameplay camera distance with a configurable multiplier.",
                false,
                HookTarget::Pattern(kPattern, kOffset),
                reinterpret_cast<std::uintptr_t>(&CameraDistance_Detour),
                jst::hooks::Slot::CameraDistance,
                RuntimeFloatConfig{}) {}

} // namespace jst::tweaks
