#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace jst::core {

[[nodiscard]] constexpr wchar_t FoldCVarNameCharacter(
    wchar_t character) noexcept {
    return character >= L'A' && character <= L'Z'
        ? static_cast<wchar_t>(character - L'A' + L'a')
        : character;
}

struct CVarNameHash {
    using is_transparent = void;

    [[nodiscard]] size_t operator()(std::wstring_view value) const noexcept {
        size_t hash = sizeof(size_t) == 8
            ? static_cast<size_t>(14695981039346656037ull)
            : static_cast<size_t>(2166136261u);
        const size_t prime = sizeof(size_t) == 8
            ? static_cast<size_t>(1099511628211ull)
            : static_cast<size_t>(16777619u);
        for (const wchar_t character : value) {
            hash ^= static_cast<size_t>(FoldCVarNameCharacter(character));
            hash *= prime;
        }
        return hash;
    }

    [[nodiscard]] size_t operator()(const std::wstring& value) const noexcept {
        return (*this)(std::wstring_view(value));
    }
};

struct CVarNameEqual {
    using is_transparent = void;

    [[nodiscard]] constexpr bool operator()(
        std::wstring_view left,
        std::wstring_view right) const noexcept {
        if (left.size() != right.size()) {
            return false;
        }
        for (size_t index = 0; index < left.size(); ++index) {
            if (FoldCVarNameCharacter(left[index]) !=
                FoldCVarNameCharacter(right[index])) {
                return false;
            }
        }
        return true;
    }
};

} // namespace jst::core
