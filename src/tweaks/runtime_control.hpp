#pragma once

#include "slider_utils.hpp"

#include <functional>
#include <string_view>
#include <variant>
#include <vector>

namespace jst::tweaks {

// All string_view fields below must point at storage that outlives the
// RuntimeControl vector returned by GetRuntimeControls(). In practice every
// callsite uses string literals or owning std::string members of the tweak,
// both of which are stable for the tweak lifetime.

/// A slider that mutates a single float value (e.g. a hook multiplier or a
/// sharpening strength).  `current` is a per-frame snapshot; the overlay
/// passes `&current` to ImGui::SliderFloat and calls `apply(current)` when
/// the value changes.
struct SliderFloatControl {
    std::string_view label;
    FloatSliderSpec spec;
    float current = 0.0f;
    std::function<void(float)> apply;
    std::string_view configSection;
    std::string_view configKey;
    std::string_view tooltip;
};

[[nodiscard]] inline SliderFloatControl MakeSliderFloatControl(
    FloatSliderSpec spec,
    float current,
    std::function<void(float)> apply,
    std::string_view label,
    std::string_view configSection,
    std::string_view configKey,
    std::string_view tooltip = {}) {
    spec.defaultValue = DefaultSliderValue(spec);
    return SliderFloatControl{
        .label = label,
        .spec = spec,
        .current = LoadSliderValue(current, spec),
        .apply = std::move(apply),
        .configSection = configSection,
        .configKey = configKey,
        .tooltip = tooltip,
    };
}

// Snap rawValue onto the spec grid and call apply() when it differs from the
// tweak-owned baseline captured before ImGui mutated control.current.
[[nodiscard]] inline bool TryCommitSliderEdit(SliderFloatControl& control,
                                              float rawValue,
                                              float persistedBaseline) {
    const float normalized = NormalizeFloatSlider(rawValue, control.spec);
    control.current = normalized;
    if (SliderValuesNearlyEqual(normalized, persistedBaseline)) {
        return false;
    }
    control.apply(control.current);
    return true;
}

// Idle frame: restore the widget value from tweak-owned state.
inline void RestoreSliderBaseline(SliderFloatControl& control, float persistedBaseline) {
    control.current = persistedBaseline;
}

/// A boolean toggle that mutates a single bool value (e.g. a CVar enable
/// flag).  `current` is a per-frame snapshot; the overlay passes `&current`
/// to ImGui::Checkbox and calls `apply(current)` when the value changes.
struct CheckboxControl {
    std::string_view          label;
    bool                      current;
    bool                      defaultValue;
    std::function<void(bool)> apply;
    std::string_view          configSection;
    std::string_view          configKey;
    std::string_view          tooltip;
};

/// A read-only informational line rendered as disabled text.  No `apply`
/// callback; returns false from RenderControl so it never triggers saves.
struct LabelControl {
    std::string_view label;
};

using RuntimeControl = std::variant<SliderFloatControl, CheckboxControl, LabelControl>;

} // namespace jst::tweaks