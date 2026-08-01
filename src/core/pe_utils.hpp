#pragma once

#include "pe_types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <windows.h>

namespace jst::core::utils {

    struct ModuleSection {
        uint8_t* base = nullptr;
        size_t size = 0;
    };

    // Validates if an address points to committed, readable memory.
    [[nodiscard]] bool IsValidPointer(uintptr_t addr);

    // Validates if an address is within executable memory.
    [[nodiscard]] bool IsExecutablePointer(uintptr_t addr);

    // Retrieves information about a specific section (e.g., ".text", ".rdata") within the game module.
    [[nodiscard]] std::optional<ModuleSection> GetModuleSection(const ModuleInfo& modInfo, const char* sectionName);

    // Helper functions for safe memory reads
    [[nodiscard]] int32_t SafeReadInt32(uintptr_t addr);
    [[nodiscard]] float SafeReadFloat(uintptr_t addr);
    [[nodiscard]] uintptr_t SafeReadPointer(uintptr_t addr);

} // namespace jst::core::utils
