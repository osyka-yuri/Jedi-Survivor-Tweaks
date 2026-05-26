#pragma once

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string_view>

namespace jst::core::detail {

// Tokens recognised as boolean false / true, matched case-insensitively.
// The arrays themselves are lower-case; MatchesAnyIgnoreCase lifts the input
// to lower-case during comparison.
constexpr std::string_view kFalseLiterals[] { "0", "false", "no" };
constexpr std::string_view kTrueLiterals[]  { "1", "true",  "yes" };

inline bool EqualIgnoreCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

template <std::ranges::input_range R>
inline bool MatchesAny(std::string_view sv, const R& set) {
    return std::ranges::any_of(set, [sv](std::string_view candidate) {
        return EqualIgnoreCase(sv, candidate);
    });
}

constexpr std::string_view Trim(std::string_view sv) {
    const auto start = sv.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end = sv.find_last_not_of(" \t\r\n");
    return sv.substr(start, end - start + 1);
}

} // namespace jst::core::detail
