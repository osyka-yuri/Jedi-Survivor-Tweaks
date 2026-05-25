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

    template<typename T>
    bool SafeWrite(uintptr_t addr, T val) {
        if (!addr) return false;
        DWORD oldProtect;
        if (VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *reinterpret_cast<T*>(addr) = val;
            VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T), oldProtect, &oldProtect);
            return true;
        }
        return false;
    }

    // Writes `val` to both `addr` and `addr + sizeof(T)` under a single VirtualProtect pair.
    // Use for adjacent primary + shadow fields to halve the syscall count (2 instead of 4).
    template<typename T>
    bool SafeWritePair(uintptr_t addr, T val) {
        if (!addr) return false;
        constexpr size_t span = sizeof(T) * 2;
        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void*>(addr), span, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;
        *reinterpret_cast<T*>(addr)             = val;
        *reinterpret_cast<T*>(addr + sizeof(T)) = val;
        VirtualProtect(reinterpret_cast<void*>(addr), span, oldProtect, &oldProtect);
        return true;
    }

} // namespace jst::core::utils
