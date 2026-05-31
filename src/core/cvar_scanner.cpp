#include "cvar_scanner.hpp"
#include "pe_utils.hpp"
#include "logging.hpp"

#include <cstring>
#include <cwchar>
#include <algorithm>
#include <chrono>

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
constexpr int kRefVarRadius = 64;

// Bytes back from the current instruction that are excluded from the ref-var
// search to avoid spuriously matching the name-string LEA itself.
constexpr int kRefVarExclusion = 5;

// ---------------------------------------------------------------------------

uint8_t* FindCVarStringAddress(std::wstring_view name, const utils::ModuleSection& rdataSec) {
    const size_t bytes = name.size() * sizeof(wchar_t);
    if (rdataSec.size < bytes + sizeof(wchar_t)) return nullptr;

    const wchar_t* pStart = reinterpret_cast<const wchar_t*>(rdataSec.base);
    const size_t maxChars = (rdataSec.size - bytes - sizeof(wchar_t)) / sizeof(wchar_t) + 1;
    const wchar_t first = name[0];

    const wchar_t* curr = pStart;
    const wchar_t* end = pStart + maxChars;

    while (curr < end) {
        curr = static_cast<const wchar_t*>(std::wmemchr(curr, first, end - curr));
        if (!curr) break;

        if (std::memcmp(curr, name.data(), bytes) == 0 &&
            curr[name.size()] == L'\0') {
            return reinterpret_cast<uint8_t*>(const_cast<wchar_t*>(curr));
        }
        ++curr;
    }
    return nullptr;
}

// Scans .rdata and .data for a 3-pointer registration record whose first word
// matches `targetPtrVal` (the VA of the CVar name string).
// Accepts pre-fetched sections from ScanForNames to avoid redundant PE walks.
uintptr_t FindStaticRegistration(
    uintptr_t targetPtrVal,
    const ModuleInfo& modInfo,
    const utils::ModuleSection& rdataSec,
    const std::optional<utils::ModuleSection>& dataSec)
{
    const utils::ModuleSection* sections[] = {
        &rdataSec,
        dataSec ? &dataSec.value() : nullptr
    };

    for (const auto* sec : sections) {
        if (!sec) continue;
        for (size_t offset = 0; offset + 24 <= sec->size; offset += 8) {
            const uintptr_t* ptr = reinterpret_cast<const uintptr_t*>(sec->base + offset);
            if (ptr[0] != targetPtrVal) continue;
            const uintptr_t refValCandidate = ptr[1];
            const uintptr_t helpCandidate   = ptr[2];
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

} // anonymous namespace

std::vector<ScanEntry> ScanForNames(std::span<const std::wstring_view> names, const ModuleInfo& mod) {
    const auto rdataSec = utils::GetModuleSection(mod, ".rdata");
    const auto textSec  = utils::GetModuleSection(mod, ".text");
    if (!rdataSec || !textSec) return {};

    // .data is needed by FindStaticRegistration; fetch once here.
    const auto dataSec = utils::GetModuleSection(mod, ".data");

    JST_LOG_INFO("ScanForNames: processing {} names", names.size());
    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<ScanEntry> entries;
    entries.reserve(names.size());

    for (auto name : names) {
        uint8_t* str = FindCVarStringAddress(name, *rdataSec);
        if (str) {
            ScanEntry entry;
            entry.name    = std::wstring(name);
            entry.strAddr = reinterpret_cast<uintptr_t>(str);
            entry.strRVA  = entry.strAddr - mod.base;

            // Search for a static registration record in .rdata/.data.
            // Pass pre-fetched sections to avoid re-walking the PE header.
            const uintptr_t staticRef =
                FindStaticRegistration(entry.strAddr, mod, *rdataSec, dataSec);
            if (staticRef) {
                entry.staticRefCandidates.push_back(staticRef);
            }

            entries.push_back(std::move(entry));
        }
    }
    if (entries.empty()) return {};

    auto t1 = std::chrono::high_resolution_clock::now();
    JST_LOG_INFO("ScanForNames: string & static search took {} ms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    // Build a sorted vector from string RVA to entries so one .text pass handles all names.
    std::vector<std::pair<uintptr_t, ScanEntry*>> rvaMap;
    rvaMap.reserve(entries.size());
    for (auto& e : entries) {
        rvaMap.push_back({e.strRVA, &e});
    }
    std::sort(rvaMap.begin(), rvaMap.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    for (size_t i = 0; i + 10 <= textSec->size; ++i) {
        const uint8_t* p = textSec->base + i;

        // Recognise LEA r64, [rip + disp32] or MOV r64, imm64
        const bool isLea      = (p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && ((p[2] & 0xC7) == 0x05);
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

        auto it = std::lower_bound(rvaMap.begin(), rvaMap.end(), targetRVA,
            [](const auto& pair, uintptr_t rva) { return pair.first < rva; });
        if (it == rvaMap.end() || it->first != targetRVA) continue;

        const uintptr_t matchAddr = reinterpret_cast<uintptr_t>(p);
        const int instLen = isLea ? 7 : 10;

        for (auto matchIt = it; matchIt != rvaMap.end() && matchIt->first == targetRVA; ++matchIt) {
            auto* e = matchIt->second;
            // Guard against common short strings (e.g. "0", "1") that match too many sites.
            if (e->globalPtrCandidates.size() > 100) continue;

            // Forward scan: look for a MOV that reads or writes a global pointer.
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
                e->globalPtrCandidates.push_back(candidateGP);
            }

            // Lateral scan: look for a nearby LEA that loads a reference variable.
            if (e->refVarCandidates.empty() && e->globalPtrCandidates.size() < 100) {
                for (int offset = -kRefVarRadius; offset <= kRefVarRadius; ++offset) {
                    if (offset >= -kRefVarExclusion && offset <= instLen) continue;
                    const uint8_t* pRef = reinterpret_cast<const uint8_t*>(matchAddr + offset);
                    if (pRef < textSec->base || pRef + 7 > textSec->base + textSec->size) continue;

                    const bool isRefLea = (pRef[0] >= 0x48 && pRef[0] <= 0x4F) &&
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

    auto t2 = std::chrono::high_resolution_clock::now();
    JST_LOG_INFO("ScanForNames: .text scan took {} ms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count());

    return entries;
}

} // namespace jst::core