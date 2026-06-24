; tweak_hooks.asm - Naked detours for JediSurvivorTweaks
; Assemble with ml64.exe (MASM x64)
;
; JstContext layout (pinned by static_asserts in src/hooks/hook_context.hpp):
;   [r11 + 0]  uintptr_t  resumeAddress   (8 bytes)
;   [r11 + 8]  float      multiplier      (4 bytes)
;   [r11 + 12] float      one             (4 bytes, constant 1.0f)
;   [r11 + 16] uint64_t   payload0        (8 bytes, slot-specific payload)
;   [r11 + 24] <padding>                  (8 bytes, alignas(16) pads 24 -> 32)
;   sizeof = 32, alignment = 16
;
; SLOT_* below must match the order of jst::hooks::Slot in hook_context.hpp.

EXTERN g_contexts : QWORD

CONTEXT_SIZE EQU 32
SLOT_LETTERBOXPILLARBOXFIX EQU 0
SLOT_GAMEPLAYFOV      EQU 1
SLOT_CAMERADISTANCE   EQU 2
SLOT_ASPECTRATIOUIFIX EQU 3
SLOT_STREAMINGPOOLFIX EQU 4

.CODE

; ---------------------------------------------------------------------------
; Macros for common naked detour boilerplate.
;
; JST_DETOUR_PROLOGUE slot_idx
;   Reserves 8 bytes on the stack for the return address (sub rsp, 8) so that 
;   the original stack alignment is preserved during the detour execution, and
;   the resume address can be safely popped by `ret`.
;   Saves r11 (used as context pointer) and rcx (often live/clobbered).
;   Loads the context pointer into r11 and stores the resume address into
;   the reserved stack slot.
;
; JST_DETOUR_EPILOGUE
;   Restores rcx and r11, then issues `ret` which safely jumps back to the 
;   resumeAddress placed at [rsp] by the detour body, perfectly restoring 
;   original registers and stack pointer.
; ---------------------------------------------------------------------------
JST_DETOUR_PROLOGUE MACRO slot_idx
    sub rsp, 8
    push r11
    push rcx

    lea r11, [g_contexts + slot_idx * CONTEXT_SIZE]
    mov rcx, qword ptr [r11 + 0]
    mov qword ptr [rsp + 16], rcx   ; store resume address in the reserved slot
ENDM

JST_DETOUR_EPILOGUE MACRO
    pop rcx
    pop r11
    ret
ENDM

; ---------------------------------------------------------------------------
; AspectRatioUIFix
;   The patched instruction holds a UI/HUD scale proportional to render height
;   (~height/1440: 0.75 at 1920x1080, 0.8333 at 1920x1200, 1.1111 at 2560x1600).
;   Because it is height-based, a 16:10 display -- 10/9 taller than 16:9 at the
;   same width -- renders the UI 10/9 too large. The detour SCALES that value by
;   Context::multiplier ([r11+8]) and re-runs the original 14-byte sequence:
;       movaps xmm4, xmm0
;       divss  xmm4, [rbp+1F8h]
;       movaps xmm0, xmm4
;   A factor of 0.9 (= 9/10) maps any 16:10 resolution onto its 16:9 equivalent
;   (0.8333*0.9=0.75, 1.1111*0.9=1.0); 1.0 is a no-op, safe on any aspect ratio.
;   mulss touches no GP register and no rflags, so only r11/rcx need saving.
; ---------------------------------------------------------------------------
AspectRatioUIFix_Detour PROC PUBLIC
    JST_DETOUR_PROLOGUE SLOT_ASPECTRATIOUIFIX

    mulss xmm0, dword ptr [r11 + 8] ; scale the UI value by the configured factor
    movaps xmm4, xmm0               ; original 14-byte sequence follows
    divss xmm4, dword ptr [rbp+000001F8h]
    movaps xmm0, xmm4

    JST_DETOUR_EPILOGUE
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
    JST_DETOUR_PROLOGUE SLOT_LETTERBOXPILLARBOXFIX

    mov eax, [rbx + 02BCh]          ; replicate: copy display width
    mov [rdi + 030h], eax           ; replicate: write to render context
    mov dword ptr [rbx + 02C8h], 0  ; clear constraint flag in game memory
    mov eax, 0                      ; return zero (no constraint) in eax

    JST_DETOUR_EPILOGUE
LetterboxPillarboxFix_Detour ENDP

; ---------------------------------------------------------------------------
; GameplayFOV - applies configurable FOV multiplier.
; ---------------------------------------------------------------------------
GameplayFOV_Detour PROC PUBLIC
    JST_DETOUR_PROLOGUE SLOT_GAMEPLAYFOV

    movss xmm0, dword ptr [r11 + 8]
    subss xmm0, dword ptr [r11 + 12]
    movaps xmm6, xmm0
    movaps xmm7, xmm6

    JST_DETOUR_EPILOGUE
GameplayFOV_Detour ENDP

; ---------------------------------------------------------------------------
; CameraDistance - applies configurable camera distance multiplier.
; ---------------------------------------------------------------------------
CameraDistance_Detour PROC PUBLIC
    JST_DETOUR_PROLOGUE SLOT_CAMERADISTANCE

    movss xmm6, dword ptr [r11 + 8]
    movss xmm7, dword ptr [r11 + 12]

    JST_DETOUR_EPILOGUE
CameraDistance_Detour ENDP

; ---------------------------------------------------------------------------
; StreamingPoolFix - locks the streaming pool size to a configured number of GBs.
;
; The 16-byte patch window (kAbsoluteJmpSize=14 -> HDE consumes 4 instructions
; here) starting at the pattern's kOffset covers, verbatim:
;   patch+0 : 48 8B 54 24 40       mov rdx, [rsp+40h]        (5)  -- pool size
;   patch+5 : 84 C0                test al, al               (2)  -- flags from CALL
;   patch+7 : 74 16                jz +0x16                  (2)  -- skip recalc
;   patch+9 : 48 8B 83 08 01 00 00 mov rax, [rbx+0108h]      (7)  -- recalc path
;   patch+16: <resume>                                       -- next original instr
;
; The pattern pins every one of these 16 bytes literally, so the jz target is
; fixed: IP after the jz is patch+9, target = patch+9+0x16 = patch+31. With
; resume = patch+16, the taken path must land at resume+15. The detour replaces
; instruction 1 with a load from payload0 and replicates 2-4 faithfully,
; exposing two exit paths that both return through the prologue's reserved slot.
; ---------------------------------------------------------------------------
StreamingPoolFix_Detour PROC PUBLIC
    JST_DETOUR_PROLOGUE SLOT_STREAMINGPOOLFIX

    mov  rdx, qword ptr [r11 + 16]      ; (1) replacement: locked pool size from payload0
    test al, al                         ; (2) replicate: flags from the preceding CALL
    jz   taken                          ; (3) replicate: jz +0x16 -> skip recalc path
    mov  rax, qword ptr [rbx + 0108h]   ; (4) replicate: recalc path (fall-through only)

    JST_DETOUR_EPILOGUE                 ; ret -> resume (patch+16)

taken:
    ; Same register restore as the epilogue, but the resume address in the
    ; reserved slot points at patch+16; the jz wants patch+31, so add the
    ; 15-byte delta before `ret` pops it. rcx/r11 are still on the stack
    ; (nothing in the body pops them), so the pop order matches the epilogue.
    pop  rcx
    pop  r11
    add  qword ptr [rsp], 15            ; resume + 0x0F -> patch+31 (jz target)
    ret
StreamingPoolFix_Detour ENDP

END
