#pragma once

#include "cvar_layout.hpp"
#include "cvar_name.hpp"

#include <array>
#include <string_view>

namespace jst::core {

struct CVarOverride {
    std::wstring_view name;
    CVarReadLayout readLayout{};
};

inline constexpr std::array<CVarOverride, 1> kCVarOverrides{{
    CVarOverride{
        .name = L"respawn.InterpolatedRendering",
        .readLayout = CVarReadLayout{
            .kind = CVarStorageKind::Inline,
            .valueOffset = 0x50,
        },
    },
}};

[[nodiscard]] inline const CVarOverride* FindCVarOverride(
    std::wstring_view name) noexcept {
    const CVarNameEqual equal;
    for (const auto& entry : kCVarOverrides) {
        if (equal(entry.name, name)) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace jst::core
