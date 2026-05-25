#include "cvar_scanner.hpp"
#include "pe_utils.hpp"

#include <cstring>
#include <unordered_map>

namespace jst::core {

namespace {

// ---------------------------------------------------------------------------
// Scan-window constants
// ---------------------------------------------------------------------------

// How many bytes forward from the name-reference instruction to search for a
// companion MOV that loads the CVar's global-pointer variable.
constexpr int kFwdScanBytes = 40;

// Half-width (in bytes) of the search window used to find a nearby LEA that
// captures the CVar's reference variable (IConsoleVariable**).
constexpr int kRefVarRadius = 30;

// Bytes back from the current instruction that are excluded from the ref-var
// search to avoid spuriously matching the name-string LEA itself.
constexpr int kRefVarExclusion = 5;

// ---------------------------------------------------------------------------

uint8_t* FindCVarStringAddress(std::wstring_view name, const utils::ModuleSection& rdataSec) {
    const size_t bytes = name.size() * sizeof(wchar_t);
    if (rdataSec.size < bytes + sizeof(wchar_t)) return nullptr;
    // Wide-char strings in .rdata are always 2-byte aligned; stride by wchar_t to halve iterations.
    // First-char pre-check avoids the full memcmp for most positions.
    const wchar_t first = name[0];
    for (size_t i = 0; i + bytes + sizeof(wchar_t) <= rdataSec.size; i += sizeof(wchar_t)) {
        if (*reinterpret_cast<const wchar_t*>(rdataSec.base + i) != first) continue;
        if (std::memcmp(rdataSec.base + i, name.data(), bytes) != 0) continue;
        if (*reinterpret_cast<const wchar_t*>(rdataSec.base + i + bytes) == L'\0')
            return rdataSec.base + i;
    }
    return nullptr;
}

} // anonymous namespace

std::vector<ScanEntry> ScanForNames(std::span<const std::wstring_view> names, const ModuleInfo& mod) {
    const auto rdataSec = utils::GetModuleSection(mod, ".rdata");
    const auto textSec  = utils::GetModuleSection(mod, ".text");
    if (!rdataSec || !textSec) return {};

    std::vector<ScanEntry> entries;
    entries.reserve(names.size());

    for (auto name : names) {
        uint8_t* str = FindCVarStringAddress(name, *rdataSec);
        if (str) {
            entries.push_back({
                std::wstring(name),   // own the string
                reinterpret_cast<uintptr_t>(str),
                reinterpret_cast<uintptr_t>(str) - mod.base,
                {},
                {}
            });
        }
    }
    if (entries.empty()) return {};

    // Build a map from string RVA → entries so one .text pass handles all names.
    std::unordered_map<uintptr_t, std::vector<ScanEntry*>> rvaMap;
    rvaMap.reserve(entries.size());
    for (auto& e : entries) {
        rvaMap[e.strRVA].push_back(&e);
    }

    for (size_t i = 0; i + 10 <= textSec->size; ++i) {
        const uint8_t* p = textSec->base + i;

        // Recognise the two instruction forms that embed a pointer to the name string:
        //   LEA r64, [rip + disp32]   — 48/4C 8D /5
        //   MOV r64, imm64            — 48/49 B8-BF
        const bool isLea     = (p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && ((p[2] & 0xC7) == 0x05);
        const bool isMovImm64 = (p[0] == 0x48 || p[0] == 0x49) && (p[1] >= 0xB8 && p[1] <= 0xBF);
        if (!isLea && !isMovImm64) continue;

        uintptr_t targetRVA = 0;
        if (isLea) {
            const int32_t disp = *reinterpret_cast<const int32_t*>(p + 3);
            targetRVA = reinterpret_cast<uintptr_t>(p + 7 + disp) - mod.base;
        } else {
            const uintptr_t imm64 = *reinterpret_cast<const uintptr_t*>(p + 2);
            targetRVA = imm64 - mod.base;
        }

        auto it = rvaMap.find(targetRVA);
        if (it == rvaMap.end()) continue;

        const uintptr_t matchAddr = reinterpret_cast<uintptr_t>(p);
        const int instLen = isLea ? 7 : 10;

        for (auto* e : it->second) {
            // Forward scan: look for a MOV that reads or writes a global pointer.
            // Such an instruction is typically generated alongside the LEA when
            // UE registers a CVar (e.g. "mov rcx, [g_CVar]" or "mov [g_CVar], rax").
            for (int offset = instLen; offset <= instLen + kFwdScanBytes; ++offset) {
                const uint8_t* pMov = reinterpret_cast<const uint8_t*>(matchAddr + offset);
                if (pMov < textSec->base || pMov + 7 > textSec->base + textSec->size) continue;

                const bool isMovWrite = (pMov[0] >= 0x48 && pMov[0] <= 0x4F) &&
                                         pMov[1] == 0x89 &&
                                        ((pMov[2] & 0xC7) == 0x05);
                const bool isMovRead  = (pMov[0] >= 0x48 && pMov[0] <= 0x4F) &&
                                         pMov[1] == 0x8B &&
                                        ((pMov[2] & 0xC7) == 0x05);
                if (!isMovWrite && !isMovRead) continue;

                const int32_t dispMov       = *reinterpret_cast<const int32_t*>(pMov + 3);
                const uintptr_t candidateGP = reinterpret_cast<uintptr_t>(pMov + 7 + dispMov);
                const uintptr_t candidateObj = utils::SafeReadPointer(candidateGP);
                if (candidateObj && ValidateCVarObject(candidateObj, mod)) {
                    const int32_t val72 = utils::SafeReadInt32(candidateObj + cvar_layout::kValueOffset);
                    e->globalPtrCandidates.push_back({candidateGP, val72});
                }
            }

            // Lateral scan: look for a nearby LEA that loads a reference variable
            // (IConsoleVariable**), used as an alternative resolution path.
            if (e->refVarCandidates.empty()) {
                for (int offset = -kRefVarRadius; offset <= kRefVarRadius; ++offset) {
                    if (offset >= -kRefVarExclusion && offset <= instLen) continue;
                    const uint8_t* pRef = reinterpret_cast<const uint8_t*>(matchAddr + offset);
                    if (pRef < textSec->base || pRef + 7 > textSec->base + textSec->size) continue;
                    const bool isRefLea =
                        (pRef[0] >= 0x48 && pRef[0] <= 0x4F) &&
                         pRef[1] == 0x8D &&
                        ((pRef[2] & 0xC7) == 0x05);
                    if (!isRefLea) continue;
                    const int32_t dispRef = *reinterpret_cast<const int32_t*>(pRef + 3);
                    e->refVarCandidates.push_back(reinterpret_cast<uintptr_t>(pRef + 7 + dispRef));
                    break;
                }
            }
        }
    }

    return entries;
}

} // namespace jst::core
