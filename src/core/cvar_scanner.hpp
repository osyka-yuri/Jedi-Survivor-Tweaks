#pragma once

#include "cvar_layout.hpp"
#include "pe_types.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jst::core {

struct ScanEntry {
    std::wstring name;
    uintptr_t strAddr = 0;
    uintptr_t strRVA = 0;
    std::vector<uintptr_t> globalPtrCandidates;
    std::vector<uintptr_t> refVarCandidates;
    std::vector<uintptr_t> staticRefCandidates;
};

// Scans the game module's .rdata and .text sections to locate FConsoleVariable
// objects for the given CVar names.  Returns one ScanEntry per name that was
// found in .rdata; names absent from .rdata are silently omitted.
[[nodiscard]] std::vector<ScanEntry> ScanForNames(
    std::span<const std::wstring_view> names,
    const ModuleInfo& mod);

} // namespace jst::core
