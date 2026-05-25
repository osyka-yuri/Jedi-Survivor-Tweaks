#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace jst::core {

struct CVarOverride {
    bool bypassObjectValidation = false;
    std::optional<int32_t> valueOffset;
    std::optional<uintptr_t> knownGlobalPtrRva;
};

// Transparent hash for std::wstring keys that also accepts std::wstring_view lookups
// without allocating a temporary string. Both overloads use the same hash algorithm.
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

    [[nodiscard]] const CVarOverride* Find(std::wstring_view name) const {
        auto it = m_overrides.find(name);
        return (it != m_overrides.end()) ? &it->second : nullptr;
    }

private:
    CVarOverrideTable() {
        RegisterDefaults();
    }

    void RegisterDefaults() {
        CVarOverride o;
        o.bypassObjectValidation = true;
        o.valueOffset = 0x50;
        m_overrides.emplace(L"respawn.InterpolatedRendering", std::move(o));
    }

    std::unordered_map<std::wstring, CVarOverride, WStringHash, std::equal_to<>> m_overrides;
};

} // namespace jst::core
