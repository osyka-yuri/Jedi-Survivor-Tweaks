#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace jst::core {

struct CVarOverride {
    bool bypassObjectValidation = false;
    std::optional<int32_t>   valueOffset;
    std::optional<uintptr_t> knownGlobalPtrRva;
};

// Transparent hash for std::wstring keys that also accepts std::wstring_view
// lookups without allocating a temporary string.
struct WStringHash {
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::wstring_view sv) const noexcept {
        return std::hash<std::wstring_view>{}(sv);
    }
    [[nodiscard]] size_t operator()(const std::wstring& s) const noexcept {
        return std::hash<std::wstring_view>{}(s);
    }
};

class CVarOverrideTable {
public:
    [[nodiscard]] static CVarOverrideTable& Instance() {
        static CVarOverrideTable table;
        return table;
    }

    // Heterogeneous lookup: accepts wstring_view without allocating a temporary.
    [[nodiscard]] const CVarOverride* Find(std::wstring_view name) const {
        auto it = m_overrides.find(name);
        return (it != m_overrides.end()) ? &it->second : nullptr;
    }

private:
    CVarOverrideTable() {
        m_overrides.emplace(L"respawn.InterpolatedRendering",
            CVarOverride{.bypassObjectValidation = true, .valueOffset = 0x50});
    }

    std::unordered_map<std::wstring, CVarOverride, WStringHash, std::equal_to<>> m_overrides;
};

} // namespace jst::core
