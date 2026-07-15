; tweak_hooks.asm - Naked detours for JediSurvivorTweaks
; Assemble with ml64.exe (MASM x64)
;
; JstContext layout (pinned by static_asserts in src/hooks/hook_context.hpp):
;   [r11 + 0]  uintptr_t  resumeAddress   (8 bytes)
;   [r11 + 8]  float      multiplier      (4 bytes)
;   [r11 + 12] float      one             (4 bytes, constant 1.0f)
;   [r11 + 16] uint64_t   lockedBytes          (8 bytes, forced lock)
;   [r11 + 24] uint64_t   captureCeilingBytes  (8 bytes, capture ceiling)
;   [r11 + 32] uint64_t   fallbackBytes        (8 bytes, auto fallback)
;   [r11 + 40] <padding>                  (8 bytes, alignas(16) pads 40 -> 48)
;   sizeof = 48, alignment = 16
;
; SLOT_* constants are generated from src/hooks/slots.def.

EXTERN g_contexts : QWORD

CONTEXT_SIZE EQU 48
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
; StreamingPoolFix - locks the streaming pool size (bytes in lockedBytes).
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
;   lockedBytes != 0  → forced lock: rdx = lockedBytes
;   lockedBytes == 0  → passthrough: rdx = original [rsp+40h]
;                       lock-once CAS when size is capturable
;                    so Auto works even when r.Streaming.PoolSize stays -1.
;   captureCeilingBytes → dynamic inclusive capture ceiling in bytes
;   fallbackBytes       → effective auto fallback in bytes
;
; Capture policy (must match streaming_pool_protocol.hpp):
;   min = 0x20000000   (0.5 GiB) — reject below (passthrough, no lock)
;   max = captureCeilingBytes (70% dedicated VRAM or legacy 12 GiB)
;   above max               — use fallbackBytes for this invocation, do not lock
;
; Stack: prologue push r11 shifts original [rsp+40h] → [rsp+48h].
; Capture path pushes rax (preserve al from CALL) → engine size at [rsp+50h].
; ---------------------------------------------------------------------------
StreamingPoolFix_Detour PROC PUBLIC
    JST_DETOUR_PROLOGUE SLOT_STREAMINGPOOLFIX

    mov  rdx, qword ptr [r11 + 16]      ; lockedBytes: forced lock?
    test rdx, rdx
    jnz  have_payload

    ; ---- Auto unlocked: passthrough + optional lock-once capture ----
    push rax                            ; preserve al (CALL result for test al,al)
    mov  rdx, qword ptr [rsp + 50h]     ; original pool size [rsp+40h]

    ; rdx >= 0.5 GiB?
    mov  rax, 20000000h
    cmp  rdx, rax
    jb   skip_capture

    ; rdx > active ceiling? Preserve auto as unlocked, but never forward an
    ; absurd candidate to the game. The controller will lock fallbackBytes on its
    ; timeout if no valid CVar/detour source arrives.
    mov  rax, qword ptr [r11 + 24]
    cmp  rdx, rax
    jbe  do_capture
    mov  rdx, qword ptr [r11 + 32]      ; fallbackBytes: temporary safe value
    jmp  skip_capture

do_capture:
    ; lock cmpxchg [lockedBytes], rdx with expected=0
    xor  eax, eax
    lock cmpxchg qword ptr [r11 + 16], rdx
    jz   skip_capture                   ; we won: rdx already has the lock value
    mov  rdx, rax                       ; lost race: use winner's lockedBytes

skip_capture:
    pop  rax

have_payload:
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
