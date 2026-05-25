#include "cvar_resolver.hpp"
#include "cvar_scanner.hpp"
#include "pe_utils.hpp"

#include <cmath>
#include <cstring>

namespace jst::core {

namespace {

// ---------------------------------------------------------------------------
// Candidate-scoring helpers
// ---------------------------------------------------------------------------

// Returns true for 32-bit patterns that are ubiquitous as float defaults
// (0.0, 0.5, 1.0, 2.0, 3.0, 4.0, 5.0, 9.0, 100.0, -0.5, -1.0, -2.0,
// -9.0, -100.0).  Such values are poor discriminators between candidates.
bool IsCommonFloatDefault(uint32_t u) {
    switch (u) {
        case 0x00000000: case 0x3F000000: case 0x3F800000:
        case 0x40000000: case 0x40400000: case 0x40800000:
        case 0x40A00000: case 0x41200000: case 0x42C80000:
        case 0xBF000000: case 0xBF800000: case 0xC0000000:
        case 0xC1200000: case 0xC2C80000:
            return true;
    }
    return false;
}

// Returns true for small integer defaults that appear in many CVars.
bool IsCommonIntDefault(int32_t v) {
    switch (v) {
        case 0: case 1: case 2: case 3: case 4: case 5:
        case 6: case 7: case 8: case 9: case 10:
        case 16: case 32: case 64: case 100: case 128: case 256:
        case 512: case 1024: case 2048: case 4096:
            return true;
    }
    return false;
}

// Scores the 32-bit value stored at offset kValueOffset of a candidate CVar
// object.  Lower score = more like a genuine UE4 CVar default = better candidate.
//
//   0 — common default (small int or well-known float such as 0, 1, 100.0):
//       strong evidence the address holds a real FConsoleVariable.
//   1 — plausible but less common value: moderate evidence.
//   2 — pathological value (non-finite, subnormal, huge magnitude):
//       likely NOT a valid CVar default; disqualify this candidate.
//
// The scorer is used to disambiguate when multiple global-pointer candidates
// dereference to valid-looking objects in the same .text function.  We pick
// the candidate with the lowest score and reject any with score > 1.
int ScoreValue72(int32_t value72) {
    const uint32_t u = static_cast<uint32_t>(value72);
    if (IsCommonFloatDefault(u) || IsCommonIntDefault(value72)) return 0;

    const float f = *reinterpret_cast<const float*>(&value72);
    if (!std::isfinite(f) || std::isnan(f)) return 2;
    if (std::fpclassify(f) == FP_SUBNORMAL) return 2;
    if (std::abs(f) > 1.0e6f) return 2;

    if (std::abs(value72) <= 1000000) return 1;
    return 2;
}

// Searches .rdata and .data for a "3-pointer tuple" aligned at 8 bytes whose
// first element equals `targetPtrVal`:
//
//   [0]: targetPtrVal        — e.g. the CVar name-string VA
//   [1]: ref-variable addr   — IConsoleVariable** pointing at the CVar object
//   [2]: help-string ptr or 0
//
// This layout is the UE4 static-registration table entry produced by ECVF_*
// macros when a CVar is registered at static-init time.  Locating it gives us
// the reference variable even when no direct .text reference to the name string
// is present (e.g. when the compiler merged the string with another literal).
uintptr_t FindStaticRegistration(uintptr_t targetPtrVal, const ModuleInfo& modInfo) {
    const auto rdataSec = utils::GetModuleSection(modInfo, ".rdata");
    const auto dataSec  = utils::GetModuleSection(modInfo, ".data");

    const utils::ModuleSection* sections[] = {
        rdataSec ? &rdataSec.value() : nullptr,
        dataSec  ? &dataSec.value()  : nullptr
    };

    for (const auto* sec : sections) {
        if (!sec) continue;
        for (size_t offset = 0; offset + 24 <= sec->size; offset += 8) {
            const uintptr_t* ptr = reinterpret_cast<const uintptr_t*>(sec->base + offset);
            if (ptr[0] != targetPtrVal) continue;
            const uintptr_t refValCandidate = ptr[1];
            const uintptr_t helpCandidate   = ptr[2];
            // Pointer is from the compiler's .rdata table — committed by definition if in user space.
            // Range check avoids the VirtualQuery syscall used by IsValidPointer.
            const bool refValid  = (refValCandidate >= 0x10000 &&
                                    refValCandidate <= 0x00007FFFFFFFFFFF);
            const bool helpValid = (helpCandidate == 0 ||
                (helpCandidate >= modInfo.base && helpCandidate < modInfo.base + modInfo.size));
            if (refValid && helpValid) {
                return refValCandidate;
            }
        }
    }
    return 0;
}

// Builds a ResolvedCVar from an FConsoleVariable base address.
// Uses the override's valueOffset when provided; falls back to the standard
// kValueOffset.  Shadow is set to kShadowOffset unless the override specifies
// a custom value offset (in which case the shadow location is unknown → 0).
ResolvedCVar MakeResolvedCVar(uintptr_t obj, const CVarOverride* override) {
    const bool hasCustomOffset = override && override->valueOffset.has_value();
    const int32_t off = hasCustomOffset ? *override->valueOffset : cvar_layout::kValueOffset;
    return ResolvedCVar{
        obj + off,
        hasCustomOffset ? 0 : obj + cvar_layout::kShadowOffset,
        obj
    };
}

} // anonymous namespace

std::optional<ResolvedCVar> ResolveFromOverride(
    const CVarOverride* override,
    const ModuleInfo& mod
) {
    if (!override || !override->knownGlobalPtrRva) return std::nullopt;

    const uintptr_t gp  = mod.base + *override->knownGlobalPtrRva;
    const uintptr_t obj = utils::SafeReadPointer(gp);
    if (obj && utils::IsValidPointer(obj)) {
        return MakeResolvedCVar(obj, override);
    }
    return std::nullopt;
}

RefVarResult ResolveFromRefVar(uintptr_t refVar, const ModuleInfo& mod) {
    RefVarResult result;

    const uintptr_t possibleObj = utils::SafeReadPointer(refVar);

    // Case 1: refVar holds nullptr — FConsoleVariable not yet constructed.
    // The caller should enqueue this address and retry via the pump thread.
    if (!possibleObj) {
        result.retryable = true;
        return result;
    }

    // Case 2: refVar holds a pointer to a valid FConsoleVariable object (standard case).
    // Read both the primary value field and the shadow copy.
    if (possibleObj != refVar && ValidateCVarObject(possibleObj, mod)) {
        result.cvar = ResolvedCVar{
            possibleObj + cvar_layout::kValueOffset,
            possibleObj + cvar_layout::kShadowOffset,
            possibleObj
        };
        return result;
    }

    // Case 3: refVar holds the value directly (no object indirection).
    // Some UE4 bool/int CVars store their value at the reference variable
    // address itself rather than in a separate heap-allocated object.
    // ScoreValue72 == 0 means the bit pattern matches a common integer or
    // float default (0, 1, 2, …), which is consistent with a directly-stored
    // integer and inconsistent with a plausible heap pointer.
    const int32_t valAsInt = static_cast<int32_t>(possibleObj);
    if (ScoreValue72(valAsInt) == 0) {
        result.cvar = ResolvedCVar{refVar, 0, 0};   // no object, no shadow
        return result;
    }

    result.retryable = true;
    return result;
}

ScanResult ResolveFromScan(
    const ScanEntry& scan,
    const ModuleInfo& mod,
    const CVarOverride* override
) {
    ScanResult result;

    if (!scan.globalPtrCandidates.empty()) {
        if (override && override->bypassObjectValidation) {
            // Bypass mode: accept the first candidate whose global pointer
            // dereferences to a non-null, accessible address (used when the
            // standard vtable/flags checks are not applicable).
            for (const auto& [gp, val72] : scan.globalPtrCandidates) {
                const uintptr_t obj = utils::SafeReadPointer(gp);
                if (obj && utils::IsValidPointer(obj)) {
                    result.cvar = MakeResolvedCVar(obj, override);
                    return result;
                }
            }
        } else {
            // Normal mode: among all candidates, pick the one whose default
            // value looks most like a typical CVar (lowest ScoreValue72).
            // Reject if the best score is > 1 (no realistic CVar default found).
            size_t bestIdx   = 0;
            int    bestScore = 3;
            bool   found     = false;
            for (size_t i = 0; i < scan.globalPtrCandidates.size(); ++i) {
                const int score = ScoreValue72(scan.globalPtrCandidates[i].second);
                if (score < bestScore) {
                    bestScore = score;
                    bestIdx   = i;
                    found     = true;
                }
            }
            if (found && bestScore <= 1) {
                const uintptr_t gp  = scan.globalPtrCandidates[bestIdx].first;
                const uintptr_t obj = utils::SafeReadPointer(gp);
                if (obj && ValidateCVarObject(obj, mod)) {
                    result.cvar = MakeResolvedCVar(obj, override);
                    return result;
                }
            }
        }

        result.pendingPtr = scan.globalPtrCandidates[0].first;
        return result;
    }

    for (uintptr_t refVar : scan.refVarCandidates) {
        auto refResult = ResolveFromRefVar(refVar, mod);
        if (refResult.cvar) {
            result.cvar = std::move(*refResult.cvar);
            return result;
        }
        if (refResult.retryable && !result.pendingPtr) {
            result.pendingPtr = refVar;
        }
    }
    if (result.pendingPtr) return result;

    if (scan.strAddr) {
        const uintptr_t staticRef = FindStaticRegistration(scan.strAddr, mod);
        if (staticRef) {
            auto refResult = ResolveFromRefVar(staticRef, mod);
            if (refResult.cvar) {
                result.cvar = std::move(*refResult.cvar);
                return result;
            }
            if (refResult.retryable) {
                result.pendingPtr = staticRef;
            }
        }
    }

    return result;
}

} // namespace jst::core
