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

// Priority bitmask applied to the flags field to mark "set by console command".
// Matches ECVF_SetByConsole (priority nibble 0x0A) stored in the upper byte.
constexpr uint32_t kSetByConsole = 0x0a000000;

// Maximum plausible value for a valid flags field.
// A value above this indicates the address does not point to a valid
// FConsoleVariable (the upper byte is reserved and must be zero).
constexpr uint32_t kMaxValidFlags = 0x00FFFFFF;

} // namespace cvar_layout

// ---------------------------------------------------------------------------
// ScanEntry — one record produced by the binary scanner per CVar name
// ---------------------------------------------------------------------------
// Centralised here (rather than in cvar_scanner.hpp) so that cvar_resolver
// can include this header and use ScanEntry without pulling in all of
// cvar_scanner.hpp, which would otherwise create a circular include chain.

struct ScanEntry {
    std::wstring name;    // owns the string (avoids non-owning wstring_view lifetime hazard)
    uintptr_t strAddr = 0;  // VA of the UTF-16 name string in .rdata
    uintptr_t strRVA  = 0;  // RVA (strAddr - mod.base)

    // Candidate global-pointer addresses found near the name reference, paired
    // with the int32 value stored at offset 72 of the pointed-to object.
    // Used by the scorer to pick the most uniquely-identifiable candidate.
    std::vector<std::pair<uintptr_t, int32_t>> globalPtrCandidates;

    // Candidate reference-variable addresses (LEA targets near the name LEA).
    std::vector<uintptr_t> refVarCandidates;
};

// ---------------------------------------------------------------------------
// ValidateCVarObject — inline memory-layout validator
// ---------------------------------------------------------------------------
// Returns true iff `cvarObject` plausibly points to an FConsoleVariable:
//   • non-null
//   • vtable pointer lies within the game module's address range
//   • flags field is within the valid range (upper byte == 0)
//
// Declared inline so both cvar_scanner.cpp and cvar_resolver.cpp can call it
// after including only this header, with no link-time ambiguity.

[[nodiscard]] inline bool ValidateCVarObject(uintptr_t cvarObject, const ModuleInfo& mod) {
    if (!cvarObject) return false;
    const uintptr_t vtable = utils::SafeReadPointer(cvarObject);
    if (vtable < mod.base || vtable >= mod.base + mod.size) return false;
    const uint32_t flags = utils::SafeReadInt32(cvarObject + cvar_layout::kFlagsOffset);
    return flags <= cvar_layout::kMaxValidFlags;
}

} // namespace jst::core
