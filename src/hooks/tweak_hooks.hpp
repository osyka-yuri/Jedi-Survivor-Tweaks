#pragma once

// Forward declarations for naked detour procedures implemented in tweak_hooks.asm.
extern "C" {
    void GameplayFOV_Detour();
    void CameraDistance_Detour();
    void LetterboxPillarboxFix_Detour();
    void AspectRatioUIFix_Detour();
    void StreamingPoolFix_Detour();
}
