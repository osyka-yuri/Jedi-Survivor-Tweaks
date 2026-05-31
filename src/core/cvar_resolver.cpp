#include "cvar_resolver.hpp"
#include "cvar_layout.hpp"
#include "cvar_overrides.hpp"
#include "cvar_scanner.hpp"
#include "logging.hpp"
#include "pe_utils.hpp"
#include "string_utils.hpp"

namespace jst::core {

namespace {

ResolvedCVar MakeResolvedCVar(uintptr_t obj, const CVarOverride* override) {
    const bool hasCustomOffset = override && override->valueOffset.has_value() && *override->valueOffset != 0;
    const uint32_t off = hasCustomOffset ? *override->valueOffset : cvar_layout::kValueOffset;
    return ResolvedCVar{
        obj + off,
        hasCustomOffset ? 0 : obj + cvar_layout::kShadowOffset,
        obj
    };
}

// Strict whitelist of values commonly used to initialise CVars at startup.
bool IsValidCVarDefault(int32_t valAsInt) {
    if (valAsInt == 0) return true;
    if (valAsInt > 0 && valAsInt <= 4096) return true;
    if (valAsInt >= -100 && valAsInt < 0) return true;

    const float valAsFloat = *reinterpret_cast<const float*>(&valAsInt);
    if (std::isfinite(valAsFloat) && !std::isnan(valAsFloat) &&
        valAsFloat >= -10000.0f && valAsFloat <= 10000.0f) {
        return true;
    }

    switch (valAsInt) {
        case 0x3F800000: // 1.0f
        case 0x40000000: // 2.0f
        case 0x40400000: // 3.0f
        case 0x40800000: // 4.0f
        case 0x41200000: // 10.0f
        case 0x42C80000: // 100.0f
            return true;
        default:
            return false;
    }
}

} // anonymous namespace

std::optional<ResolvedCVar> ResolveFromOverride(const CVarOverride* override, const ModuleInfo& mod) {
    if (!override || !override->knownGlobalPtrRva) return std::nullopt;

    const uintptr_t gp  = mod.base + *override->knownGlobalPtrRva;
    const uintptr_t obj = utils::SafeReadPointer(gp);
    if (obj && utils::IsValidPointer(obj)) {
        return MakeResolvedCVar(obj, override);
    }
    return std::nullopt;
}

std::optional<ResolvedCVar> ResolveFromScan(const ScanEntry& scan, const ModuleInfo& mod, const CVarOverride* override) {
    JST_LOG_DEBUG("'{}': Starting resolution. Candidates: gp={}, ref={}, static={}",
                  utils::WideToUtf8(scan.name),
                  scan.globalPtrCandidates.size(),
                  scan.refVarCandidates.size(),
                  scan.staticRefCandidates.size());

    // Cache section bounds once per call — IsSafeDataPointer would otherwise
    // re-walk the PE header for every candidate checked.
    const auto dataSec = utils::GetModuleSection(mod, ".data");
    const auto bssSec  = utils::GetModuleSection(mod, ".bss");

    // Returns true if ptr is non-null, in the module range, and in .data/.bss.
    const auto isSafePtr = [&](uintptr_t ptr) -> bool {
        if (!ptr || ptr < mod.base || ptr >= mod.base + mod.size) return false;
        const uintptr_t base = ptr;
        if (dataSec) {
            const auto lo = reinterpret_cast<uintptr_t>(dataSec->base);
            if (base >= lo && base < lo + dataSec->size) return true;
        }
        if (bssSec) {
            const auto lo = reinterpret_cast<uintptr_t>(bssSec->base);
            if (base >= lo && base < lo + bssSec->size) return true;
        }
        return false;
    };

    const bool bypass = override && override->bypassObjectValidation;

    // -----------------------------------------------------------------------
    // 1. Global pointer candidates — MOV instructions near the name LEA
    // -----------------------------------------------------------------------
    for (uintptr_t gp : scan.globalPtrCandidates) {
        const uintptr_t obj = utils::SafeReadPointer(gp);
        if (!obj) {
            JST_LOG_DEBUG("'{}': gp 0x{:X} -> null, skipping.", utils::WideToUtf8(scan.name), gp);
            continue;
        }
        if (bypass || ValidateCVarObject(obj, mod)) {
            const int32_t val72 = utils::SafeReadInt32(obj + cvar_layout::kValueOffset);
            if (bypass || IsValidCVarDefault(val72)) {
                JST_LOG_DEBUG("'{}': Resolved via gp 0x{:X} -> obj 0x{:X}.",
                              utils::WideToUtf8(scan.name), gp, obj);
                return MakeResolvedCVar(obj, override);
            }
            JST_LOG_DEBUG("'{}': obj 0x{:X} failed IsValidCVarDefault (val72={}).",
                          utils::WideToUtf8(scan.name), obj, val72);
        } else {
            JST_LOG_DEBUG("'{}': obj 0x{:X} failed ValidateCVarObject.",
                          utils::WideToUtf8(scan.name), obj);
        }
    }

    // -----------------------------------------------------------------------
    // 2. Reference variable candidates — LEA instructions near the name LEA
    // -----------------------------------------------------------------------
    for (uintptr_t refVar : scan.refVarCandidates) {
        if (!isSafePtr(refVar)) continue;

        const uintptr_t possibleObj = utils::SafeReadPointer(refVar);

        // 2a. refVar holds a pointer to the FConsoleVariable heap object.
        if (possibleObj && possibleObj != refVar &&
            (bypass || ValidateCVarObject(possibleObj, mod))) {
            const int32_t val72 = utils::SafeReadInt32(possibleObj + cvar_layout::kValueOffset);
            if (bypass || IsValidCVarDefault(val72)) {
                return MakeResolvedCVar(possibleObj, override);
            }
        }

        // 2b. refVar holds the primitive value directly (FAutoConsoleVariableRef-style).
        //     Skip the null case: possibleObj == 0 means the engine hasn't constructed
        //     the object yet, not that the CVar value is zero.
        if (possibleObj != 0) {
            const int32_t valAsInt = static_cast<int32_t>(possibleObj);
            if (bypass || IsValidCVarDefault(valAsInt)) {
                return ResolvedCVar{refVar, 0, 0};
            }
        }
    }

    // -----------------------------------------------------------------------
    // 3. Static registration array candidates
    // -----------------------------------------------------------------------
    for (uintptr_t staticRef : scan.staticRefCandidates) {
        if (!isSafePtr(staticRef)) continue;

        const uintptr_t possibleObj = utils::SafeReadPointer(staticRef);
        if (possibleObj && possibleObj != staticRef &&
            (bypass || ValidateCVarObject(possibleObj, mod))) {
            const int32_t val72 = utils::SafeReadInt32(possibleObj + cvar_layout::kValueOffset);
            if (bypass || IsValidCVarDefault(val72)) {
                return MakeResolvedCVar(possibleObj, override);
            }
        }
    }

    if (scan.strAddr) {
        JST_LOG_INFO("'{}': string at 0x{:X} — no valid candidate found.",
                     utils::WideToUtf8(scan.name), scan.strAddr);
    }

    return std::nullopt;
}

} // namespace jst::core
