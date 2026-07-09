#include "aspect_ratio_ui_fix.hpp"

#include "hooks/tweak_hooks.hpp"
#include "slider_specs.hpp"

namespace {

constexpr const char* kHudPattern =
    "0F 28 F0 48 8B 05 ? ? ? ? 0F 28 C6 F3 0F 11 35 ? ? ? ? "
    "0F 28 74 24 20 48 89 05 ? ? ? ? C6 05 ? ? ? ? 01 "
    "48 83 C4 30 5B C3";
constexpr const char* kMenuPattern =
    "0F 28 E0 F3 0F 5E A5 F8 01 00 00 0F 28 C4";

} // namespace

namespace jst::tweaks {

AspectRatioUIFix::AspectRatioUIFix()
    : HookTweak(
          "AspectRatioUIFix",
          "Scales the HUD, 3D map markers, and menus to correct oversized UI "
          "on 16:10 displays.",
          false,
          std::vector<HookBinding>{
              HookBinding{
                  .siteName = "AspectRatioUIFix.Hud",
                  // ReplayOriginal over the 5-byte movaps prologue so the detour
                  // scales xmm0 before the relocated instruction stores it.
                  .target = HookTarget::Pattern(
                      kHudPattern,
                      0,
                      5,
                      core::HookContinuation::ReplayOriginal),
                  .detour =
                      reinterpret_cast<std::uintptr_t>(&AspectRatioUIFix_Hud_Detour),
                  .slot = jst::hooks::Slot::AspectRatioUIHud,
              },
              HookBinding{
                  .siteName = "AspectRatioUIFix.Menu",
                  .target = HookTarget::Pattern(kMenuPattern, 0, 11),
                  .detour =
                      reinterpret_cast<std::uintptr_t>(&AspectRatioUIFix_Menu_Detour),
                  .slot = jst::hooks::Slot::AspectRatioUIMenu,
              },
          },
          RuntimeFloatConfig{
              .slider = kAspectRatioSliderSpec,
              .sliderTooltip =
                  "UI scale multiplier applied to the HUD, 3D markers, and menus. "
                  "0.9 corrects 16:10; 1.0 leaves the game scale unchanged.",
          }) {}

} // namespace jst::tweaks
