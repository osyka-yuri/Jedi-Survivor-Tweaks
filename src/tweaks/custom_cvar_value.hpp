#pragma once

#include "core/ini_helpers.hpp"

#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace jst::tweaks {

/**
 * Validates the numeric-only custom-CVar syntax without inferring the actual
 * engine CVar type. Booleans normalize to 1/0; finite numeric text is returned
 * verbatim (apart from surrounding INI whitespace), including exponent form.
 */
[[nodiscard]] inline std::optional<std::string> NormalizeCustomCVarValue(
    std::string_view raw) {
    const std::string_view value = jst::core::detail::Trim(raw);
    if (jst::core::detail::EqualIgnoreCase(value, "true")) {
        return "1";
    }
    if (jst::core::detail::EqualIgnoreCase(value, "false")) {
        return "0";
    }
    if (value.empty()) {
        return std::nullopt;
    }

    const std::string_view parsed = value.front() == '+'
        ? value.substr(1)
        : value;
    if (parsed.empty()) {
        return std::nullopt;
    }

    double number = 0.0;
    const auto [end, error] = std::from_chars(
        parsed.data(),
        parsed.data() + parsed.size(),
        number,
        std::chars_format::general);
    if (error != std::errc{} ||
        end != parsed.data() + parsed.size() ||
        !std::isfinite(number)) {
        return std::nullopt;
    }
    return std::string(value);
}

} // namespace jst::tweaks
