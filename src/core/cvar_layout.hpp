#pragma once

// Single source of truth for Unreal Engine FConsoleVariable memory layout.
// All byte offsets and flag values are derived from binary analysis of the
// Jedi: Survivor shipping executable.  Every translation unit that touches
// raw CVar memory should include this header instead of redeclaring constants.
//
// Also hosts ScanEntry (the scanner's output record) and ValidateCVarObject
// (an inline layout validator) so that cvar_scanner and cvar_resolver can
// both use them without creating a circular include dependency.

#include "pe_utils.hpp"     // SafeReadPointer, SafeReadInt32, ModuleSection
#include "pe_types.hpp"     // ModuleInfo

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jst::core {

// ---------------------------------------------------------------------------
// UE FConsoleVariable layout constants
// ---------------------------------------------------------------------------
namespace cvar_layout {

// Offset of the 32-bit primary value field inside an FConsoleVariable object.
// The engine reads this field at runtime; writing it applies the CVar change.
constexpr int32_t kValueOffset = 72;    // 0x48

// Offset of the 32-bit shadow (initial/default) copy.
// Some UE codepaths cache the console-set value separately from the in-memory
// default; writing both fields ensures the change survives soft-resets.
constexpr int32_t kShadowOffset = 76;   // 0x4C

// Offset of the 32-bit console-variable flags field.
constexpr int32_t kFlagsOffset = 0x18;

// The upper byte records the source priority (constructor through console).
// Lower bits contain the persistent EConsoleVariableFlags values.
constexpr uint32_t kFlagBitsMask = 0x00FFFFFF;
constexpr uint32_t kSetByMask = 0xFF000000;
constexpr uint32_t kSetByConsole = 0x0a000000;

// Offset of the external pointer in FConsoleVariableRef objects.
// Used to locate the backing variable for FAutoConsoleVariableRef.
constexpr int32_t kRefOffset = 32;      // 0x20

} // namespace cvar_layout

// ---------------------------------------------------------------------------
// ScanEntry — one record produced by the binary scanner per CVar name
// ---------------------------------------------------------------------------
// Centralised here (rather than in cvar_scanner.hpp) so that cvar_resolver
// can include this header and use ScanEntry without pulling in all of
// cvar_scanner.hpp, which would otherwise create a circular include chain.

struct ScanEntry {
    std::wstring name;      // owns the string (avoids non-owning wstring_view lifetime hazard)
    uintptr_t strAddr = 0;  // VA of the UTF-16 name string in .rdata
    uintptr_t strRVA  = 0;  // RVA (strAddr - mod.base)

    // Candidate global-pointer addresses found near the name reference.
    std::vector<uintptr_t> globalPtrCandidates;

    // Candidate reference-variable addresses (LEA targets near the name LEA).
    std::vector<uintptr_t> refVarCandidates;

    // Candidate reference-variable addresses from static registration scanning.
    std::vector<uintptr_t> staticRefCandidates;
};

// ---------------------------------------------------------------------------
// ValidateCVarObject — inline memory-layout validator
// ---------------------------------------------------------------------------
// Returns true iff `cvarObject` plausibly points to an FConsoleVariable:
//   • non-null
//   • vtable pointer lies within committed, accessible memory
//   • flags field is within the valid range (upper byte == 0)
//
// The `mod` parameter is intentionally unused: vtables may belong to engine
// DLLs (renderer, audio, etc.) loaded outside the main module range, so we
// accept any committed address rather than gating on mod.base/mod.size.
//
// Declared inline so both cvar_scanner.cpp and cvar_resolver.cpp can call it
// after including only this header, with no link-time ambiguity.

[[nodiscard]] inline bool ValidateCVarObject(uintptr_t cvarObject, const ModuleInfo& /*mod*/) {
    if (!cvarObject) return false;
    const uintptr_t vtable = utils::SafeReadPointer(cvarObject);
    if (!utils::IsValidPointer(vtable)) return false;
    const uint32_t flags = utils::SafeReadInt32(cvarObject + cvar_layout::kFlagsOffset);
    const uint32_t priority = flags & cvar_layout::kSetByMask;
    return priority <= cvar_layout::kSetByConsole;
}

} // namespace jst::core
