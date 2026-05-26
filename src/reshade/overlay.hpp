#pragma once

// Forward-declare the ReShade type so this header stays slim and does not pull
// in the full reshade.hpp in translation units that only need the declaration.
namespace reshade::api { struct effect_runtime; }

namespace jst {

/// ImGui overlay callback registered with reshade::register_overlay.
/// Called every frame on the render thread while the ReShade UI is open and
/// the "JediSurvivorTweaks" panel is visible.
void DrawOverlay(reshade::api::effect_runtime* runtime);

} // namespace jst
