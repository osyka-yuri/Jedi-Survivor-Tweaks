#pragma once

#include "cvar_layout.hpp"
#include "pe_types.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace jst::core {

struct CVarOverride;
struct ScanEntry;

// The resolved write targets for a single CVar.
struct ResolvedCVar {
    uintptr_t cvarObject = 0;
    uintptr_t stringSetter = 0;
    CVarReadLayout readLayout{};
};

[[nodiscard]] bool ValidateResolvedCVar(
    const ResolvedCVar& resolved) noexcept;
[[nodiscard]] uintptr_t ResolveCVarReadAddress(
    const ResolvedCVar& resolved) noexcept;

// Attempts to resolve a CVar from dynamic scan results (.rdata string references).
[[nodiscard]] std::optional<ResolvedCVar> ResolveFromScan(
    const ScanEntry& scan,
    const ModuleInfo& mod,
    const CVarOverride* override);

} // namespace jst::core
