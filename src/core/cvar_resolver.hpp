#pragma once

#include "pe_utils.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace jst::core {

struct CVarOverride;
struct ScanEntry;

// The resolved write targets for a single CVar.
struct ResolvedCVar {
    uintptr_t writeAddr = 0;
    uintptr_t writeAddrShadow = 0;
    uintptr_t cvarObject = 0;
};

// Attempts to resolve a CVar via an explicit override (e.g., specific build signatures).
[[nodiscard]] std::optional<ResolvedCVar> ResolveFromOverride(
    const CVarOverride* override,
    const ModuleInfo& mod);

// Attempts to resolve a CVar from dynamic scan results (.rdata string references).
[[nodiscard]] std::optional<ResolvedCVar> ResolveFromScan(
    const ScanEntry& scan,
    const ModuleInfo& mod,
    const CVarOverride* override);

} // namespace jst::core
