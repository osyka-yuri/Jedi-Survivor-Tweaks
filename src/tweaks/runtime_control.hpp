#pragma once

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
    std::string_view           label;
    float                      min;
    float                      max;
    float                      current;                 // snapshot at GetRuntimeControls() call
    float                      defaultValue;            // for Reset button
    std::function<void(float)> apply;                   // overlay calls on slider change
    std::string_view           configSection;           // for Save: which [section]
    std::string_view           configKey;               // for Save: which key
    std::string_view           tooltip;                 // optional ImGui tooltip text; empty == no tooltip
};

/// A boolean toggle that mutates a single bool value (e.g. a CVar enable
/// flag).  `current` is a per-frame snapshot; the overlay passes `&current`
/// to ImGui::Checkbox and calls `apply(current)` when the value changes.
struct CheckboxControl {
    std::string_view          label;
    bool                      current;
    bool                      defaultValue;            // for Reset button
    std::function<void(bool)> apply;
    std::string_view          configSection;
    std::string_view          configKey;
    std::string_view          tooltip;                 // empty == no tooltip
};

/// A read-only informational line rendered as disabled text.  No `apply`
/// callback; returns false from RenderControl so it never triggers saves.
struct LabelControl {
    std::string_view label;
};

using RuntimeControl = std::variant<SliderFloatControl, CheckboxControl, LabelControl>;

} // namespace jst::tweaks
