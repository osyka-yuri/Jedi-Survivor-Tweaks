#pragma once

#include "cvar_layout.hpp"
#include "cvar_overrides.hpp"

#include <cstdint>
#include <optional>

namespace jst::core {

// The resolved write targets for a single CVar.
// Produced by ResolveFromOverride, ResolveFromRefVar, or ResolveFromScan.
struct ResolvedCVar {
    // Address of the primary value field (int32 or float).
    // Always non-zero for a valid resolution.
    uintptr_t writeAddr = 0;

    // Address of the shadow (initial-value) copy.
    // Zero means no shadow exists for this CVar and only writeAddr is written.
    // Non-zero means both writeAddr and writeAddrShadow receive the new value.
    uintptr_t writeAddrShadow = 0;

    // Base address of the FConsoleVariable object.
    // Used by UpdateFlags() to set the ECVF_SetByConsole priority bit.
    // Zero for "direct-value" CVars where the value lives at the reference
    // variable itself rather than inside a heap-allocated object.
    uintptr_t cvarObject = 0;
};

// Returned by ResolveFromRefVar when the reference variable was found in the
// binary but the CVar object is not yet constructed.
struct RefVarResult {
    std::optional<ResolvedCVar> cvar;

    // True when resolution failed but may succeed later (CVar not yet
    // constructed).  The caller should enqueue the name for retry via the
    // background pump thread.
    bool retryable = false;
};

struct ScanResult {
    std::optional<ResolvedCVar> cvar;
    std::optional<uintptr_t>    pendingPtr;
};

[[nodiscard]] std::optional<ResolvedCVar> ResolveFromOverride(
    const CVarOverride* override,
    const ModuleInfo& mod);

[[nodiscard]] RefVarResult ResolveFromRefVar(uintptr_t refVar, const ModuleInfo& mod);

[[nodiscard]] ScanResult ResolveFromScan(
    const ScanEntry& scan,
    const ModuleInfo& mod,
    const CVarOverride* override);

} // namespace jst::core
