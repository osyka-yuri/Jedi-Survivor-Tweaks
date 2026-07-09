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
#pragma warning(push)
#pragma warning(disable: 4324)  // JstContext is padded due to alignas(16). The
                                // 8 bytes of tail padding are intentional: the
                                // asm indexes g_contexts with CONTEXT_SIZE=32,
                                // and per-element 16-byte alignment requires
                                // sizeof to be a multiple of 16. The padding is
                                // verified by the static_assert(sizeof==32) below.
struct alignas(16) JstContext {
    std::uintptr_t resumeAddress;  // [r11 + 0]  jumped to by `ret` in detours
    float          multiplier;     // [r11 + 8]  configured per-tweak value
    float          one;            // [r11 + 12] constant 1.0f -- used by the
                                   //            GameplayFOV detour to compute
                                   //            (multiplier - 1) via `subss`
    uint64_t       payload0;       // [r11 + 16] slot-specific (e.g., StreamingPoolFix stores pool bytes here)
};
#pragma warning(pop)
} // extern "C"

// Layout pins -- tweak_hooks.asm relies on every one of these. Any change to
// the struct must be mirrored in the asm or the detours will read garbage.
static_assert(sizeof(JstContext) == 32, "tweak_hooks.asm expects CONTEXT_SIZE EQU 32");
static_assert(alignof(JstContext) == 16, "tweak_hooks.asm assumes 16-byte alignment");
static_assert(offsetof(JstContext, resumeAddress) == 0, "tweak_hooks.asm uses [r11 + 0]");
static_assert(offsetof(JstContext, multiplier) == 8,         "tweak_hooks.asm uses [r11 + 8]");
static_assert(offsetof(JstContext, one) == 12,               "tweak_hooks.asm uses [r11 + 12]");
static_assert(offsetof(JstContext, payload0) == 16,          "tweak_hooks.asm uses [r11 + 16]");
static_assert(offsetof(JstContext, payload0) % 8 == 0,       "payload0 must be 8-byte aligned for atomic writes");

namespace jst::hooks {

using Context = JstContext;

enum class Slot : std::uint32_t {
#define JST_HOOK_SLOT(name) name,
#include "slots.def"
#undef JST_HOOK_SLOT
    Count
};

static_assert(std::to_underlying(Slot::Count) == 6);

} // namespace jst::hooks

extern "C" {
alignas(16) extern JstContext g_contexts[std::to_underlying(jst::hooks::Slot::Count)];
} // extern "C"

namespace jst::hooks {

inline Context& GetContext(Slot s) noexcept {
    return g_contexts[std::to_underlying(s)];
}

} // namespace jst::hooks
