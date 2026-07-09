#pragma once

extern "C" {
    void GameplayFOV_Detour();
    void CameraDistance_Detour();
    void LetterboxPillarboxFix_Detour();
    void AspectRatioUIFix_Hud_Detour();
    void AspectRatioUIFix_Menu_Detour();
    void StreamingPoolFix_Detour();
}
