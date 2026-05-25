#pragma once

#include <cstddef>
#include <cstdint>

namespace jst::core {

// Lightweight PE-module descriptor, shared between memory_scanner and pe_utils.
// Lives in its own header so pe_utils does not need to include memory_scanner
// (and its scanner API) just to spell this type.
struct ModuleInfo {
    uintptr_t base = 0;
    size_t    size = 0;
};

} // namespace jst::core
