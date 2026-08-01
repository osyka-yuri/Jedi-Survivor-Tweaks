#pragma once

#include "core/cvar_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string_view>

namespace cvar_test {

struct FakeCVar;
using SetterCallback = void (*)(FakeCVar&);

struct FakeCVar {
    uintptr_t* vtable = nullptr;
    std::array<std::byte, 16> beforeFlags{};
    uint32_t flags = 0;
    std::array<std::byte, 4> beforeReference{};
    void* externalValue = nullptr;
    std::array<std::byte, 32> beforeInlineValue{};
    union {
        int32_t inlineValue = 0;
        float inlineFloat;
    };
    int32_t shadowValue = 0;
    int32_t customValue = 0;

    uint32_t setterCalls = 0;
    uint32_t lastSetBy = 0;
    bool ignoreSetBy = false;
    bool targetIsFloat = false;
    void* setterTarget = nullptr;
    SetterCallback callback = nullptr;
    std::array<wchar_t, 64> lastValue{};
};

static_assert(offsetof(FakeCVar, flags) ==
              jst::core::cvar_layout::kFlagsOffset);
static_assert(offsetof(FakeCVar, externalValue) ==
              jst::core::cvar_layout::kRefOffset);
static_assert(offsetof(FakeCVar, inlineValue) ==
              jst::core::cvar_layout::kValueOffset);
static_assert(offsetof(FakeCVar, shadowValue) ==
              jst::core::cvar_layout::kShadowOffset);
static_assert(offsetof(FakeCVar, customValue) == 0x50);

inline void __fastcall FakeStringSetter(
    uintptr_t rawObject,
    const wchar_t* value,
    uint32_t setBy) {
    auto& object = *reinterpret_cast<FakeCVar*>(rawObject);
    ++object.setterCalls;
    object.lastSetBy = setBy;
    if (!object.ignoreSetBy) {
        object.flags =
            (object.flags & jst::core::cvar_layout::kFlagBitsMask) | setBy;
    }
    (void)wcsncpy_s(
        object.lastValue.data(),
        object.lastValue.size(),
        value ? value : L"",
        _TRUNCATE);

    if (object.setterTarget) {
        if (object.targetIsFloat) {
            *static_cast<float*>(object.setterTarget) =
                value ? std::wcstof(value, nullptr) : 0.0f;
        } else {
            *static_cast<int32_t*>(object.setterTarget) =
                value ? static_cast<int32_t>(std::wcstol(value, nullptr, 10)) : 0;
        }
    }
    if (object.callback) {
        object.callback(object);
    }
}

inline void __fastcall FakeFloatSetter(
    uintptr_t rawObject,
    float value,
    uint32_t setBy) {
    auto& object = *reinterpret_cast<FakeCVar*>(rawObject);
    ++object.setterCalls;
    object.lastSetBy = setBy;
    if (!object.ignoreSetBy) {
        object.flags =
            (object.flags & jst::core::cvar_layout::kFlagBitsMask) | setBy;
    }
    if (object.setterTarget) {
        *static_cast<float*>(object.setterTarget) = value;
    }
    if (object.callback) {
        object.callback(object);
    }
}

inline void __fastcall FakeIntSetter(
    uintptr_t rawObject,
    int32_t value,
    uint32_t setBy) {
    auto& object = *reinterpret_cast<FakeCVar*>(rawObject);
    ++object.setterCalls;
    object.lastSetBy = setBy;
    if (!object.ignoreSetBy) {
        object.flags =
            (object.flags & jst::core::cvar_layout::kFlagBitsMask) | setBy;
    }
    if (object.setterTarget) {
        *static_cast<int32_t*>(object.setterTarget) = value;
    }
    if (object.callback) {
        object.callback(object);
    }
}

struct FakeVTable {
    FakeVTable() {
        slots[jst::core::cvar_layout::kVtableSetString] =
            reinterpret_cast<uintptr_t>(&FakeStringSetter);
        slots[jst::core::cvar_layout::kVtableSetFloat] =
            reinterpret_cast<uintptr_t>(&FakeFloatSetter);
        slots[jst::core::cvar_layout::kVtableSetInt] =
            reinterpret_cast<uintptr_t>(&FakeIntSetter);
    }

    std::array<uintptr_t, 24> slots{};
};

static_assert(std::tuple_size_v<decltype(FakeVTable::slots)> >
              jst::core::cvar_layout::kVtableSetInt);

inline void Bind(FakeCVar& object, FakeVTable& vtable) {
    object.vtable = vtable.slots.data();
}

[[nodiscard]] inline bool LastValueEquals(
    const FakeCVar& object,
    std::wstring_view expected) {
    return std::wstring_view(object.lastValue.data()) == expected;
}

} // namespace cvar_test
