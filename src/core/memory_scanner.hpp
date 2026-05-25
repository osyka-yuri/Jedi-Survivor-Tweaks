#pragma once

#include "pe_types.hpp"

#include <cstdint>
#include <expected>
#include <vector>
#include <string>
#include <optional>
#include <span>
#include <string_view>

namespace jst::core {

[[nodiscard]] std::optional<ModuleInfo> GetGameModuleInfo();

// Pre-parsed byte-pattern, suitable for batch scanning.
//   bytes[i] is the expected literal when mask[i] == true.
//   firstAnchorOffset is the index of the first non-wildcard byte (used to
//   pivot one-pass scans on a known literal anchor).
struct ParsedPattern {
    std::vector<uint8_t> bytes;
    std::vector<bool>    mask;
    size_t               firstAnchorOffset = 0;   // index of first masked byte
    uint8_t              firstAnchorByte   = 0;   // bytes[firstAnchorOffset]
    bool                 allWildcards      = false;
};

[[nodiscard]] std::expected<ParsedPattern, std::string> ParsePattern(std::string_view pattern);

// Pattern: bytes with wildcard '?', e.g. "48 89 5C 24 ? 48 89 74 24 ? 57"
[[nodiscard]] std::optional<uintptr_t> FindPattern(std::span<const std::byte> region, const std::string& pattern);
[[nodiscard]] std::optional<uintptr_t> FindPatternInModule(const std::string& pattern);

// Locate every parsed pattern in a single linear pass over `region`.
// Output is index-correspondent with the input span (nullopt = not found).
// Uses a first-anchor-byte map for O(1) candidate dispatch per byte position.
[[nodiscard]] std::vector<std::optional<uintptr_t>>
FindPatternsBatch(std::span<const ParsedPattern> patterns, std::span<const std::byte> region);

// Byte-level scan for exact sequence
[[nodiscard]] std::optional<uintptr_t> FindBytes(std::span<const std::byte> region, const std::vector<uint8_t>& bytes);

} // namespace jst::core
