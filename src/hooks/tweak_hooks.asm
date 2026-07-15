; tweak_hooks.asm - Naked detours for JediSurvivorTweaks
; Assemble with ml64.exe (MASM x64)
;
; JstContext layout (pinned by static_asserts in src/hooks/hook_context.hpp):
;   [r11 + 0]  uintptr_t  resumeAddress   (8 bytes)
;   [r11 + 8]  float      multiplier      (4 bytes)
;   [r11 + 12] float      one             (4 bytes, constant 1.0f)
;   [r11 + 16] uint64_t   forcedBytes               (8 bytes, C++-owned lock)
;   [r11 + 24] uint64_t   captureCeilingBytes       (8 bytes, capture ceiling)
;   [r11 + 32] uint64_t   fallbackBytes             (8 bytes, auto fallback)
;   [r11 + 40] uint64_t   firstObservedEngineBytes  (8 bytes, first in-range sample)
;   sizeof = 48, alignment = 16
;
; SLOT_* constants are generated from src/hooks/slots.def.

EXTERN g_contexts : QWORD

CONTEXT_SIZE EQU 48
STREAMING_FORCED_OFFSET EQU 16
STREAMING_CEILING_OFFSET EQU 24
STREAMING_FALLBACK_OFFSET EQU 32
STREAMING_FIRST_OBSERVED_OFFSET EQU 40
STREAMING_MIN_BYTES EQU 20000000h
INCLUDE tweak_hooks_slots.inc

.CODE

; ---------------------------------------------------------------------------
; Macros for common naked detour boilerplate.
;
; JST_DETOUR_PROLOGUE slot_idx
;   Saves r11 and loads the context pointer into r11.
;
; JST_DETOUR_EPILOGUE
;   Exchanges the saved r11 with the continuation address and returns without
;   changing any other general-purpose register.
; ---------------------------------------------------------------------------
JST_DETOUR_PROLOGUE MACRO slot_idx
    push r11
    lea r11, [g_contexts + slot_idx * CONTEXT_SIZE]
ENDM

JST_DETOUR_EPILOGUE MACRO
    mov r11, qword ptr [r11 + 0]
    xchg r11, qword ptr [rsp]
    ret
ENDM

; =============================================================================
; AspectRatioUIFix.Hud
; =============================================================================
AspectRatioUIFix_Hud_Detour PROC PUBLIC
    JST_DETOUR_PROLOGUE SLOT_ASPECTRATIOUIHUD

    mulss xmm0, dword ptr [r11 + 8]

    JST_DETOUR_EPILOGUE
AspectRatioUIFix_Hud_Detour ENDP

; ---------------------------------------------------------------------------
; AspectRatioUIFix.Menu
; Fragile: [rbp+1F8h] is a fixed stack-frame offset in the owning UI function.
; Game updates that change frame layout will break this detour silently.
; ---------------------------------------------------------------------------
AspectRatioUIFix_Menu_Detour PROC PUBLIC
    JST_DETOUR_PROLOGUE SLOT_ASPECTRATIOUIMENU

    movaps xmm4, xmm0
    divss xmm4, dword ptr [rbp+000001F8h]
    mulss xmm4, dword ptr [r11 + 8]

    JST_DETOUR_EPILOGUE
AspectRatioUIFix_Menu_Detour ENDP

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
; StreamingPoolFix - locks the streaming pool size (bytes in forcedBytes).
;
; The 16-byte patch window (kAbsoluteJmpSize=14, 4 instructions decoded by Zydis)
; starting at the pattern's kOffset covers, verbatim:
;   patch+0 : 48 8B 54 24 40       mov rdx, [rsp+40h]        (5)  -- pool size
;   patch+5 : 84 C0                test al, al               (2)  -- flags from CALL
;   patch+7 : 74 16                jz +0x16                  (2)  -- skip recalc
;   patch+9 : 48 8B 83 08 01 00 00 mov rax, [rbx+0108h]      (7)  -- recalc path
;   patch+16: <resume>                                       -- next original instr
;
; Detour protocol — keep in sync with streaming_pool_controller.hpp /
; streaming_pool_protocol.hpp:
;   firstObservedEngineBytes stores the first original size in [min, ceiling],
;   including while a forced lock is active. C++ never clears it.
;   forcedBytes != 0  → forced lock: rdx = forcedBytes
;   forcedBytes == 0  → passthrough: rdx = original [rsp+40h]
;   captureCeilingBytes → dynamic inclusive capture ceiling in bytes
;   fallbackBytes       → effective auto fallback in bytes
;
; Capture policy (must match streaming_pool_protocol.hpp):
;   min = 0x20000000   (0.5 GiB) — reject below (passthrough, no lock)
;   max = captureCeilingBytes (70% dedicated VRAM or legacy 12 GiB)
;   above max               — use fallbackBytes for this invocation, do not lock
;
; Stack: prologue push r11 shifts original [rsp+40h] → [rsp+48h].
; We always push rax (preserve al from CALL) → engine size at [rsp+50h].
; ---------------------------------------------------------------------------
StreamingPoolFix_Detour PROC PUBLIC
    JST_DETOUR_PROLOGUE SLOT_STREAMINGPOOLFIX

    push rax                            ; preserve al (CALL result for test al,al)
    mov  rdx, qword ptr [rsp + 50h]     ; original pool size [rsp+40h]

    ; Capture the first in-range original engine size.
    mov  rax, qword ptr [r11 + STREAMING_FIRST_OBSERVED_OFFSET]
    test rax, rax
    jnz  sample_complete
    cmp  rdx, STREAMING_MIN_BYTES
    jb   sample_complete
    cmp  rdx, qword ptr [r11 + STREAMING_CEILING_OFFSET]
    ja   sample_complete
    xor  eax, eax
    lock cmpxchg qword ptr [r11 + STREAMING_FIRST_OBSERVED_OFFSET], rdx

sample_complete:
    mov  rax, qword ptr [r11 + STREAMING_FORCED_OFFSET]
    test rax, rax
    jz   unforced_path
    mov  rdx, rax
    jmp  payload_ready

unforced_path:
    cmp  rdx, STREAMING_MIN_BYTES
    jb   payload_ready
    cmp  rdx, qword ptr [r11 + STREAMING_CEILING_OFFSET]
    jbe  payload_ready
    mov  rdx, qword ptr [r11 + STREAMING_FALLBACK_OFFSET]

payload_ready:
    pop  rax

    test al, al                         ; (2) replicate: flags from the preceding CALL
    jz   taken                          ; (3) replicate: jz +0x16 -> skip recalc path
    mov  rax, qword ptr [rbx + 0108h]   ; (4) replicate: recalc path (fall-through only)

    JST_DETOUR_EPILOGUE                 ; ret -> resume (patch+16)

; Fragile: +15 is tied to the 16-byte patch window and jz displacement (0x16).
taken:
    mov  r11, qword ptr [r11 + 0]
    add  r11, 15                         ; resume + 0x0F -> patch+31 (jz target)
    xchg r11, qword ptr [rsp]
    ret
StreamingPoolFix_Detour ENDP

END
