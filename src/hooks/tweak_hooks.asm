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
;   3F8E38E4h = IEEE-754 single of 10/9 ~= 1.1111111. This is the game's
;   16:10 aspect constant; see kMaxUIScale10Over9 in aspect_ratio_ui_fix.cpp.
;   When xmm0 holds that constant, replace it with the configured multiplier,
;   then re-execute the original 14-byte sequence:
;       movaps xmm4, xmm0
;       divss  xmm4, [rbp+1F8h]
;       movaps xmm0, xmm4
; ---------------------------------------------------------------------------
AspectRatioUIFix_Detour PROC PUBLIC
    sub rsp, 8                      ; reserve slot for resume address
    push r11                        ; save r11
    push rax                        ; save rax
    push rcx                        ; save rcx
    pushfq                          ; save rflags (cmp below modifies them)

    lea r11, [g_contexts + SLOT_ASPECTRATIOUIFIX * CONTEXT_SIZE]
    mov rcx, qword ptr [r11 + 0]
    mov qword ptr [rsp + 32], rcx   ; store resume address in the reserved slot

    movd eax, xmm0
    cmp eax, 3F8E38E4h
    jne not_1111
    movss xmm0, dword ptr [r11 + 8]

not_1111:
    movaps xmm4, xmm0
    divss xmm4, dword ptr [rbp+000001F8h]
    movaps xmm0, xmm4

    popfq                           ; restore rflags
    pop rcx                         ; restore rcx
    pop rax                         ; restore rax
    pop r11                         ; restore r11
    ret                             ; jump to resume address
AspectRatioUIFix_Detour ENDP

; ---------------------------------------------------------------------------
; LetterboxPillarboxFix - removes pillarboxing/letterboxing by disabling aspect ratio
; constraint checks.
; ---------------------------------------------------------------------------
LetterboxPillarboxFix_Detour PROC PUBLIC
    sub rsp, 8                      ; reserve slot for resume address
    push r11                        ; save r11
    push rcx                        ; save rcx

    lea r11, [g_contexts + SLOT_LETTERBOXPILLARBOXFIX * CONTEXT_SIZE]
    mov rcx, qword ptr [r11 + 0]
    mov qword ptr [rsp + 16], rcx   ; store resume address in the reserved slot

    mov eax, [rbx + 02BCh]
    mov [rdi + 030h], eax
    movzx eax, byte ptr [rbx + 02C8h]
    mov eax, 0

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
