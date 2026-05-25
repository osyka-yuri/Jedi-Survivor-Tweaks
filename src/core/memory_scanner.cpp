#include "memory_scanner.hpp"
#include "logging.hpp"
#include <windows.h>
#include <psapi.h>
#include <array>
#include <charconv>
#include <expected>
#include <cstring>
#include <format>
#include <mutex>

#pragma comment(lib, "psapi.lib")
namespace {

// Process-wide cache for the game module info. The underlying PSAPI pair
// (GetModuleHandle + GetModuleInformation) is invariant for the lifetime of
// the host process, so we resolve it exactly once via std::call_once and hand
// out copies thereafter. Mirrors CVarSystem::GetOrFetchMod().
std::once_flag                       g_modOnce;
std::optional<jst::core::ModuleInfo> g_modCache;

// Compute (firstAnchorOffset, firstAnchorByte, allWildcards) from raw bytes/mask.
void ComputeAnchor(jst::core::ParsedPattern& p) {
    p.firstAnchorOffset = 0;
    while (p.firstAnchorOffset < p.mask.size() && !p.mask[p.firstAnchorOffset]) {
        ++p.firstAnchorOffset;
    }
    p.allWildcards = (p.firstAnchorOffset == p.mask.size());
    p.firstAnchorByte = p.allWildcards ? 0 : p.bytes[p.firstAnchorOffset];
}


std::optional<uintptr_t> FindSequence(
    std::span<const std::byte> region,
    const std::vector<uint8_t>& bytes,
    const std::vector<bool>& mask)
{
    if (region.size() < bytes.size()) return std::nullopt;

    // Find the first byte that is not wildcarded
    size_t first_non_wildcard = 0;
    while (first_non_wildcard < bytes.size() && !mask[first_non_wildcard]) {
        ++first_non_wildcard;
    }

    if (first_non_wildcard == bytes.size()) {
        // All bytes are wildcards (unlikely, but let's handle safely)
        return reinterpret_cast<uintptr_t>(region.data());
    }

    const uint8_t target_byte = bytes[first_non_wildcard];
    const size_t pattern_len = bytes.size();
    const size_t scan_end = region.size() - pattern_len;

    const uint8_t* start_ptr = reinterpret_cast<const uint8_t*>(region.data());
    const uint8_t* cur_ptr = start_ptr;
    const uint8_t* end_ptr = start_ptr + scan_end;

    while (cur_ptr <= end_ptr) {
        // memchr scans from cur_ptr + first_non_wildcard for at most
        // (end_ptr - cur_ptr + 1) - first_non_wildcard bytes. Without the
        // subtraction the read could spill `first_non_wildcard` bytes past
        // end_ptr -- formal UB and a real risk at page boundaries.
        const size_t remaining = static_cast<size_t>(end_ptr - cur_ptr) + 1;
        if (remaining <= first_non_wildcard) break;
        const uint8_t* match_ptr = static_cast<const uint8_t*>(
            std::memchr(cur_ptr + first_non_wildcard, target_byte, remaining - first_non_wildcard)
        );

        if (!match_ptr) {
            break;
        }

        // Calculate corresponding start address of the sequence
        const uint8_t* candidate = match_ptr - first_non_wildcard;
        if (candidate < cur_ptr) {
            // Safety check
            cur_ptr = match_ptr + 1;
            continue;
        }
        if (candidate > end_ptr) {
            break;
        }

        // Check the rest of the pattern
        bool matched = true;
        for (size_t j = 0; j < pattern_len; ++j) {
            if (mask[j] && candidate[j] != bytes[j]) {
                matched = false;
                break;
            }
        }

        if (matched) {
            return reinterpret_cast<uintptr_t>(candidate);
        }

        cur_ptr = candidate + 1;
    }

    return std::nullopt;
}

} // anonymous namespace

namespace jst::core {

std::expected<ParsedPattern, std::string> ParsePattern(std::string_view pattern) {
    ParsedPattern out;
    const char* p = pattern.data();
    const char* end_p = pattern.data() + pattern.size();
    while (p < end_p) {
        while (p < end_p && *p == ' ') ++p;
        if (p == end_p) break;

        if (p[0] == '?' && (p + 1 == end_p || p[1] == '?' || p[1] == ' ')) {
            out.bytes.push_back(0);
            out.mask.push_back(false);
            p += (p + 1 != end_p && p[1] == '?') ? 2 : 1;
            continue;
        }

        const char* tok = p;
        while (tok < end_p && *tok != ' ') ++tok;
        if (tok - p > 2) {
            return std::unexpected(std::format("Invalid pattern element (too long) near: {}", std::string_view(p, tok)));
        }

        uint8_t value = 0;
        auto [ptr, ec] = std::from_chars(p, tok, value, 16);
        if (ec != std::errc{} || ptr != tok) {
            return std::unexpected(std::format("Invalid pattern element near: {}", std::string_view(p, tok)));
        }
        out.bytes.push_back(value);
        out.mask.push_back(true);
        p = tok;
    }
    if (out.bytes.empty()) {
        return std::unexpected("Empty pattern");
    }
    ComputeAnchor(out);
    return out;
}

std::vector<std::optional<uintptr_t>>
FindPatternsBatch(std::span<const ParsedPattern> patterns, std::span<const std::byte> region) {
    std::vector<std::optional<uintptr_t>> results(patterns.size());
    if (patterns.empty()) return results;

    // Anchor map: anchor-byte -> list of pattern indices that share it.
    // 256 buckets indexed directly to skip hashing overhead.
    std::array<std::vector<size_t>, 256> anchorBuckets{};
    size_t maxPatternLen = 0;
    size_t remaining = patterns.size();
    for (size_t i = 0; i < patterns.size(); ++i) {
        const auto& p = patterns[i];
        if (p.allWildcards) {
            // Vacuous match at the start of the region; rare/unused, but defined.
            results[i] = reinterpret_cast<uintptr_t>(region.data());
            --remaining;
            continue;
        }
        anchorBuckets[p.firstAnchorByte].push_back(i);
        if (p.bytes.size() > maxPatternLen) maxPatternLen = p.bytes.size();
    }
    if (remaining == 0 || region.size() < maxPatternLen) return results;

    const uint8_t* base    = reinterpret_cast<const uint8_t*>(region.data());
    const size_t   scanEnd = region.size() - maxPatternLen;

    for (size_t pos = 0; pos <= scanEnd; ++pos) {
        const uint8_t curByte = base[pos];
        const auto& bucket = anchorBuckets[curByte];
        if (bucket.empty()) continue;

        for (size_t idx : bucket) {
            if (results[idx]) continue;  // already resolved
            const auto& p = patterns[idx];
            // candidate = pos - firstAnchorOffset
            if (pos < p.firstAnchorOffset) continue;
            const size_t candidate = pos - p.firstAnchorOffset;
            if (candidate + p.bytes.size() > region.size()) continue;

            bool matched = true;
            for (size_t j = 0; j < p.bytes.size(); ++j) {
                if (p.mask[j] && base[candidate + j] != p.bytes[j]) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                results[idx] = reinterpret_cast<uintptr_t>(base + candidate);
                if (--remaining == 0) return results;
            }
        }
    }
    return results;
}

std::optional<ModuleInfo> GetGameModuleInfo() {
    std::call_once(g_modOnce, [] {
        HMODULE h = GetModuleHandle(nullptr);
        if (!h) return;
        MODULEINFO modInfo = {};
        if (GetModuleInformation(GetCurrentProcess(), h, &modInfo, sizeof(modInfo))) {
            g_modCache = ModuleInfo{
                reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll),
                modInfo.SizeOfImage
            };
        }
    });
    return g_modCache;
}

std::optional<uintptr_t> FindPattern(std::span<const std::byte> region, const std::string& pattern) {
    auto parsed = ParsePattern(pattern);
    if (!parsed) {
        JST_LOG_ERROR("Pattern parsing failed: '{}'.", parsed.error());
        return std::nullopt;
    }
    return FindSequence(region, parsed->bytes, parsed->mask);
}

std::optional<uintptr_t> FindPatternInModule(const std::string& pattern) {
    auto mod = GetGameModuleInfo();
    if (!mod || mod->base == 0 || mod->size == 0) {
        JST_LOG_ERROR("Failed to get game module info.");
        return std::nullopt;
    }
    return FindPattern(
        std::span(reinterpret_cast<const std::byte*>(mod->base), mod->size),
        pattern);
}

std::optional<uintptr_t> FindBytes(std::span<const std::byte> region, const std::vector<uint8_t>& bytes) {
    std::vector<bool> mask(bytes.size(), true);
    return FindSequence(region, bytes, mask);
}

} // namespace jst::core
