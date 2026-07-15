#include "runtime_control.hpp"

#include "core/config.hpp"
#include "tweak.hpp"

#include <type_traits>

namespace jst::tweaks {

void PersistControl(const RuntimeControl& control, jst::core::Config& config) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, LabelControl>) {
            return;
        } else {
            if (value.persistence.overrideAction) {
                value.persistence.overrideAction(config);
                return;
            }
            if (value.persistence.section.empty() || value.persistence.key.empty()) {
                return;
            }
            if constexpr (std::is_same_v<T, SliderFloatControl>) {
                config.SetFloat(
                    value.persistence.section, value.persistence.key, value.current);
            } else {
                config.SetBool(
                    value.persistence.section, value.persistence.key, value.current);
            }
        }
    }, control);
}

bool ResetControlToDefault(RuntimeControl& control, jst::core::Config& config) {
    return std::visit([&](auto& value) -> bool {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, SliderFloatControl>) {
            if (!TryCommitSliderEdit(
                    value, value.spec.defaultValue, value.current)) {
                return false;
            }
            PersistControl(control, config);
            return true;
        } else if constexpr (std::is_same_v<T, CheckboxControl>) {
            if (value.current == value.defaultValue) {
                return false;
            }
            value.current = value.defaultValue;
            value.apply(value.current);
            PersistControl(control, config);
            return true;
        } else {
            return false;
        }
    }, control);
}

bool ResetTweakControls(
    ITweak& tweak,
    std::vector<RuntimeControl>& controls,
    jst::core::Config& config) {
    switch (tweak.ResetRuntimeControls(config)) {
    case RuntimeControlResetResult::Changed:
        return true;
    case RuntimeControlResetResult::Unchanged:
        return false;
    case RuntimeControlResetResult::Unsupported:
        break;
    }

    bool changed = false;
    for (auto& control : controls) {
        changed = ResetControlToDefault(control, config) || changed;
    }
    return changed;
}

} // namespace jst::tweaks
