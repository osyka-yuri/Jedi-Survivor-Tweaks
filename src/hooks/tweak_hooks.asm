; tweak_hooks.asm - Naked detours for JediSurvivorTweaks
; Assemble with ml64.exe (MASM x64)
;
; JstContext layout (pinned by static_asserts in src/hooks/hook_context.hpp):
;   [r11 + 0]  uintptr_t  resumeAddress   (8 bytes)
;   [r11 + 8]  float      multiplier      (4 bytes)
;   [r11 + 12] float      one             (4 bytes, constant 1.0f)
;   sizeof = 16, alignment = 16
;
; SLOT_* below must match the order of jst::hooks::Slot in hook_context.hpp.

EXTERN g_contexts : QWORD

CONTEXT_SIZE EQU 16
SLOT_LETTERBOXPILLARBOXFIX EQU 0
SLOT_GAMEPLAYFOV      EQU 1
SLOT_CAMERADISTANCE   EQU 2
SLOT_ASPECTRATIOUIFIX EQU 3

.CODE

; ---------------------------------------------------------------------------
; AspectRatioUIFix
;   While the tweak is enabled the detour unconditionally overwrites xmm0 with
;   the configured multiplier (Context::multiplier, [r11+8]), then re-executes
;   the original 14-byte sequence:
;       movaps xmm4, xmm0
;       divss  xmm4, [rbp+1F8h]
;       movaps xmm0, xmm4
;   Earlier builds only substituted when xmm0 exactly equalled the 16:10 aspect
;   constant (10/9 = 1.1111111, bits 3F8E38E4h). That exact-equality guard made
;   the hook a silent no-op whenever the game emitted a slightly different value,
;   so it was dropped -- the multiplier (clamped to 0.9-1.2 on the C++ side) is
;   now the absolute UI-scale numerator. Since eax and rflags are no longer
;   touched, only r11/rcx need saving (mirrors LetterboxPillarboxFix_Detour).
; ---------------------------------------------------------------------------
AspectRatioUIFix_Detour PROC PUBLIC
    sub rsp, 8                      ; reserve slot for resume address
    push r11                        ; save r11
    push rcx                        ; save rcx

    lea r11, [g_contexts + SLOT_ASPECTRATIOUIFIX * CONTEXT_SIZE]
    mov rcx, qword ptr [r11 + 0]
    mov qword ptr [rsp + 16], rcx   ; store resume address in the reserved slot

    movss xmm0, dword ptr [r11 + 8] ; unconditionally apply configured multiplier
    movaps xmm4, xmm0               ; original 14-byte sequence follows
    divss xmm4, dword ptr [rbp+000001F8h]
    movaps xmm0, xmm4

    pop rcx                         ; restore rcx
    pop r11                         ; restore r11
    ret                             ; jump to resume address
AspectRatioUIFix_Detour ENDP

; ---------------------------------------------------------------------------
; LetterboxPillarboxFix - removes pillarboxing/letterboxing by disabling the
; aspect-ratio constraint flag in the render context.
;
; Replaces the three-instruction sequence at kOffset=0x0F (15 bytes total):
;   mov eax,  [rbx+02BCh]     (6) -- copy display width to render ctx
;   mov [rdi+030h], eax       (3) -- write to render ctx
;   mov eax,  [rbx+02C8h]     (6) -- read constraint flag (we override this)
;
; The detour emulates the first two instructions verbatim, then instead of
; reading the flag it clears it in game memory and returns zero in eax.
; Writing the flag to 0 ensures that code paths not covered by this hook
; (e.g. the UI renderer reading [rbx+02C8h] directly) also see a disabled
; ---------------------------------------------------------------------------
LetterboxPillarboxFix_Detour PROC PUBLIC
    sub rsp, 8                      ; reserve slot for resume address
    push r11                        ; save r11
    push rcx                        ; save rcx

    lea r11, [g_contexts + SLOT_LETTERBOXPILLARBOXFIX * CONTEXT_SIZE]
    mov rcx, qword ptr [r11 + 0]
    mov qword ptr [rsp + 16], rcx   ; store resume address in the reserved slot

    mov eax, [rbx + 02BCh]          ; replicate: copy display width
    mov [rdi + 030h], eax           ; replicate: write to render context
    mov dword ptr [rbx + 02C8h], 0  ; clear constraint flag in game memory
    mov eax, 0                      ; return zero (no constraint) in eax

    pop rcx                         ; restore rcx
    pop r11                         ; restore r11
    ret                             ; jump to resume address
LetterboxPillarboxFix_Detour ENDP

; ---------------------------------------------------------------------------
; GameplayFOV - applies configurable FOV multiplier.
; ---------------------------------------------------------------------------
GameplayFOV_Detour PROC PUBLIC
    sub rsp, 8                      ; reserve slot for resume address
    push r11                        ; save r11
    push rcx                        ; save rcx

    lea r11, [g_contexts + SLOT_GAMEPLAYFOV * CONTEXT_SIZE]
    mov rcx, qword ptr [r11 + 0]
    mov qword ptr [rsp + 16], rcx   ; store resume address in the reserved slot

    movss xmm0, dword ptr [r11 + 8]
    subss xmm0, dword ptr [r11 + 12]
    movaps xmm6, xmm0
    movaps xmm7, xmm6

    pop rcx                         ; restore rcx
    pop r11                         ; restore r11
    ret                             ; jump to resume address
GameplayFOV_Detour ENDP

; ---------------------------------------------------------------------------
; CameraDistance - applies configurable camera distance multiplier.
; ---------------------------------------------------------------------------
CameraDistance_Detour PROC PUBLIC
    sub rsp, 8                      ; reserve slot for resume address
    push r11                        ; save r11
    push rcx                        ; save rcx

    lea r11, [g_contexts + SLOT_CAMERADISTANCE * CONTEXT_SIZE]
    mov rcx, qword ptr [r11 + 0]
    mov qword ptr [rsp + 16], rcx   ; store resume address in the reserved slot

    movss xmm6, dword ptr [r11 + 8]
    movss xmm7, dword ptr [r11 + 12]

    pop rcx                         ; restore rcx
    pop r11                         ; restore r11
    ret                             ; jump to resume address
CameraDistance_Detour ENDP

END
