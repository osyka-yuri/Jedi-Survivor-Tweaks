#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

// JstContext is shared C/asm state. The struct lives in `extern "C"` so the
// MASM detours in tweak_hooks.asm can address its fields by fixed offsets
// without name-mangling. The `using Context = JstContext;` alias below is a
// convenience for C++ call sites that prefer the namespaced name; it does not
// allow moving the struct into the namespace itself.
extern "C" {
struct alignas(16) JstContext {
    std::uintptr_t resumeAddress;  // [r11 + 0]  jumped to by `ret` in detours
    float          multiplier;     // [r11 + 8]  configured per-tweak value
    float          one;            // [r11 + 12] constant 1.0f -- used by the
                                   //            GameplayFOV detour to compute
                                   //            (multiplier - 1) via `subss`
};
} // extern "C"

// Layout pins -- tweak_hooks.asm relies on every one of these. Any change to
// the struct must be mirrored in the asm or the detours will read garbage.
static_assert(sizeof(JstContext) == 16, "tweak_hooks.asm expects CONTEXT_SIZE EQU 16");
static_assert(alignof(JstContext) == 16, "tweak_hooks.asm assumes 16-byte alignment");
static_assert(offsetof(JstContext, resumeAddress) == 0, "tweak_hooks.asm uses [r11 + 0]");
static_assert(offsetof(JstContext, multiplier) == 8,    "tweak_hooks.asm uses [r11 + 8]");
static_assert(offsetof(JstContext, one) == 12,          "tweak_hooks.asm uses [r11 + 12]");

namespace jst::hooks {

using Context = JstContext;

enum class Slot : std::uint32_t {
    LetterboxPillarboxFix = 0,
    GameplayFOV      = 1,
    CameraDistance   = 2,
    AspectRatioUIFix = 3,
    Count
};

} // namespace jst::hooks

extern "C" {
alignas(16) extern JstContext g_contexts[std::to_underlying(jst::hooks::Slot::Count)];
} // extern "C"

namespace jst::hooks {

inline Context& GetContext(Slot s) noexcept {
    return g_contexts[std::to_underlying(s)];
}

} // namespace jst::hooks
