#include "pe_utils.hpp"
#include <cstring>

namespace jst::core::utils {

    bool IsValidPointer(uintptr_t addr) {
        if (addr < 0x10000) return false;
        if (addr > 0x00007FFFFFFFFFFF) return false;
        if (addr & 3) return false;

        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
            return mbi.State == MEM_COMMIT;
        }
        return false;
    }

    bool IsExecutablePointer(uintptr_t addr) {
        if (addr < 0x10000) return false;
        if (addr > 0x00007FFFFFFFFFFF) return false;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT) return false;
        constexpr DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ
                                 | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (mbi.Protect & execMask) != 0;
    }

    std::optional<ModuleSection> GetModuleSection(const ModuleInfo& modInfo, const char* sectionName) {
        if (modInfo.base == 0) return std::nullopt;

        PIMAGE_DOS_HEADER dos = reinterpret_cast<PIMAGE_DOS_HEADER>(modInfo.base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;

        PIMAGE_NT_HEADERS nt = reinterpret_cast<PIMAGE_NT_HEADERS>(modInfo.base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return std::nullopt;

        PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            char secName[9] = {};
            std::memcpy(secName, sec->Name, 8);
            if (std::strcmp(secName, sectionName) == 0) {
                return ModuleSection{
                    reinterpret_cast<uint8_t*>(modInfo.base + sec->VirtualAddress),
                    sec->Misc.VirtualSize
                };
            }
        }
        return std::nullopt;
    }

    int32_t SafeReadInt32(uintptr_t addr) {
        __try {
            return *reinterpret_cast<int32_t*>(addr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    float SafeReadFloat(uintptr_t addr) {
        __try {
            return *reinterpret_cast<float*>(addr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0.0f;
        }
    }

    uintptr_t SafeReadPointer(uintptr_t addr) {
        __try {
            return *reinterpret_cast<uintptr_t*>(addr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

} // namespace jst::core::utils
