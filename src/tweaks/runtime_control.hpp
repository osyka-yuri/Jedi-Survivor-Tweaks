#pragma once

#include "slider_utils.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace jst::core {
class Config;
}

namespace jst::tweaks {

class ITweak;

using PersistControlOverride = std::function<void(jst::core::Config&)>;

struct ControlPersistence {
    std::string_view section;
    std::string_view key;
    PersistControlOverride overrideAction;
};

enum class RuntimeControlResetResult : uint8_t {
    Unsupported,
    Unchanged,
    Changed,
};

// All string_view fields must point at literals or tweak-owned storage that
// outlives the ephemeral control vector returned for the current frame.
struct SliderFloatControl {
    std::string_view label;
    FloatSliderSpec spec;
    float current = 0.0f;
    std::function<void(float)> apply;
    ControlPersistence persistence;
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
        .persistence = ControlPersistence{
            .section = configSection,
            .key = configKey,
        },
        .tooltip = tooltip,
    };
}

[[nodiscard]] inline bool TryCommitSliderEdit(
    SliderFloatControl& control, float rawValue, float persistedBaseline) {
    const float normalized = NormalizeFloatSlider(rawValue, control.spec);
    control.current = normalized;
    if (SliderValuesNearlyEqual(normalized, persistedBaseline)) {
        return false;
    }
    control.apply(control.current);
    return true;
}

inline void RestoreSliderBaseline(
    SliderFloatControl& control, float persistedBaseline) noexcept {
    control.current = persistedBaseline;
}

struct CheckboxControl {
    std::string_view label;
    bool current = false;
    bool defaultValue = false;
    std::function<void(bool)> apply;
    ControlPersistence persistence;
    std::string_view tooltip;
};

// Dynamic informational text owns its storage; no mutable tweak-side string
// is needed merely to keep a string_view alive through the frame.
struct LabelControl {
    std::string label;
};

using RuntimeControl = std::variant<SliderFloatControl, CheckboxControl, LabelControl>;

void PersistControl(const RuntimeControl& control, jst::core::Config& config);
[[nodiscard]] bool ResetControlToDefault(
    RuntimeControl& control, jst::core::Config& config);
[[nodiscard]] bool ResetTweakControls(
    ITweak& tweak,
    std::vector<RuntimeControl>& controls,
    jst::core::Config& config);

} // namespace jst::tweaks
