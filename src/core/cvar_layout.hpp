#pragma once

#include <cstddef>
#include <cstdint>

namespace jst::core {

// ---------------------------------------------------------------------------
// UE FConsoleVariable layout constants
// ---------------------------------------------------------------------------
namespace cvar_layout {

// Read-only diagnostics layout for the supported Jedi: Survivor build.
constexpr int32_t kValueOffset = 72;    // 0x48

// Offset of the 32-bit shadow (initial/default) copy.
// Optional render-thread shadow copy. The mod never writes either field.
constexpr int32_t kShadowOffset = 76;   // 0x4C

// Offset of the 32-bit console-variable flags field.
constexpr int32_t kFlagsOffset = 0x18;

// The upper byte records the source priority (constructor through console).
// Lower bits contain the persistent EConsoleVariableFlags values.
constexpr uint32_t kFlagBitsMask = 0x00FFFFFF;
constexpr uint32_t kSetByMask = 0xFF000000;
constexpr uint32_t kSetByScalability = 0x01000000;
constexpr uint32_t kSetByConsole = 0x09000000;

// Offset of the external pointer in FConsoleVariableRef objects.
// Used to locate the backing variable for FAutoConsoleVariableRef.
constexpr int32_t kRefOffset = 32;      // 0x20

// UE 4.26/4.27 IConsoleVariable exposes consecutive string, float and int
// Set overloads. The inherited IConsoleObject vtable occupies slots 0..15
// (including its private Release), so the overloads start at slot 16. Keep
// this explicit: an executable pointer alone cannot identify a virtual.
constexpr size_t kVtableSetString = 16;
constexpr size_t kVtableSetFloat = 17;
constexpr size_t kVtableSetInt = 18;

} // namespace cvar_layout

enum class CVarStorageKind : uint8_t {
    Inline,
    ExternalReference,
};

struct CVarReadLayout {
    CVarStorageKind kind = CVarStorageKind::Inline;
    int32_t valueOffset = cvar_layout::kValueOffset;
    int32_t referenceOffset = cvar_layout::kRefOffset;
};

} // namespace jst::core
