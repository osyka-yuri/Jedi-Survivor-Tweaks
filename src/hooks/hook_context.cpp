#include "hook_context.hpp"

extern "C" {
    alignas(16) JstContext g_contexts[std::to_underlying(jst::hooks::Slot::Count)] = {
        {0, 1.0f, 1.0f, 0},  // Slot 0: LetterboxPillarboxFix
        {0, 1.0f, 1.0f, 0},  // Slot 1: GameplayFOV
        {0, 1.0f, 1.0f, 0},  // Slot 2: CameraDistance
        {0, 1.0f, 1.0f, 0},  // Slot 3: AspectRatioUIFix
        {0, 1.0f, 1.0f, 0},  // Slot 4: StreamingPoolFix
    };
}
