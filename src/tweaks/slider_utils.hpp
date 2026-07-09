#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace jst::tweaks {

// Float slider step grid: values snap to min + k*step (k = 0..N) with index N
// pinned to spec.max. Integer indices + double arithmetic avoid 0.1f drift
// (e.g. 9.999 snapping to 9.9 instead of 10.0). step == 0 means continuous.
//
// Invariants for product specs (see slider_specs.hpp):
//   NormalizeFloatSlider(spec.max, spec) == spec.max
//   NormalizeFloatSlider(v, spec) is always in [min, max]

inline constexpr float kFloatSliderStepContinuous = 0.0f;
inline constexpr float kSliderValueEpsilon = 1e-4f;

struct FloatSliderSpec {
    float min = 0.0f;
    float max = 0.0f;
    float defaultValue = 0.0f;
    float step = kFloatSliderStepContinuous;
};

[[nodiscard]] constexpr bool IsValidSpec(const FloatSliderSpec& s) noexcept {
    return s.min <= s.max
        && s.step >= 0.0f
        && s.defaultValue >= s.min
        && s.defaultValue <= s.max;
}

[[nodiscard]] inline bool HasSliderStep(float step) noexcept {
    return step > 0.0f;
}

[[nodiscard]] inline bool SliderValuesNearlyEqual(float lhs, float rhs) noexcept {
    return std::fabs(lhs - rhs) <= kSliderValueEpsilon;
}

namespace detail {

[[nodiscard]] inline int SliderStepCount(const FloatSliderSpec& spec) noexcept {
    if (!HasSliderStep(spec.step)) {
        return 0;
    }

    const double span = static_cast<double>(spec.max) - static_cast<double>(spec.min);
    const double step = static_cast<double>(spec.step);
    if (span <= 0.0 || step <= 0.0) {
        return 0;
    }

    return static_cast<int>(std::llround(span / step));
}

[[nodiscard]] inline int ValueToStepIndex(double value, double min, double step, int maxIndex) noexcept {
    if (step <= 0.0) return 0;
    int index = static_cast<int>(std::llround((value - min) / step));
    return std::clamp(index, 0, maxIndex);
}

[[nodiscard]] inline float StepIndexToValue(
    int index, double min, double step, int maxIndex, float declaredMax) noexcept {
    if (index >= maxIndex) {
        return declaredMax;
    }
    if (index <= 0) {
        return static_cast<float>(min);
    }
    return static_cast<float>(min + static_cast<double>(index) * step);
}

[[nodiscard]] inline float SliderIndexToValue(int index, const FloatSliderSpec& spec) noexcept {
    if (!HasSliderStep(spec.step)) {
        return std::clamp(static_cast<float>(index), spec.min, spec.max);
    }

    const int maxIndex = SliderStepCount(spec);
    const int clamped = std::clamp(index, 0, maxIndex);
    return StepIndexToValue(
        clamped,
        static_cast<double>(spec.min),
        static_cast<double>(spec.step),
        maxIndex,
        spec.max);
}

[[nodiscard]] inline int SliderValueToIndex(float value, const FloatSliderSpec& spec) noexcept {
    if (!HasSliderStep(spec.step)) {
        return 0;
    }

    const float clamped = std::clamp(value, spec.min, spec.max);
    const double minD = static_cast<double>(spec.min);
    const double stepD = static_cast<double>(spec.step);
    const int maxIndex = SliderStepCount(spec);

    if (clamped >= spec.max || SliderValuesNearlyEqual(clamped, spec.max)) {
        return maxIndex;
    }

    return ValueToStepIndex(static_cast<double>(clamped), minD, stepD, maxIndex);
}

} // namespace detail

[[nodiscard]] inline float NormalizeFloatSlider(float value, const FloatSliderSpec& spec) noexcept {
    if (!HasSliderStep(spec.step)) {
        return std::clamp(value, spec.min, spec.max);
    }

    return detail::SliderIndexToValue(detail::SliderValueToIndex(value, spec), spec);
}

[[nodiscard]] inline float LoadSliderValue(float raw, const FloatSliderSpec& spec) noexcept {
    return NormalizeFloatSlider(raw, spec);
}

[[nodiscard]] inline float DefaultSliderValue(const FloatSliderSpec& spec) noexcept {
    return NormalizeFloatSlider(spec.defaultValue, spec);
}

[[nodiscard]] inline const char* SliderDisplayFormat(float step) noexcept {
    if (!HasSliderStep(step)) {
        return "%.3f";
    }

    struct Threshold { float minStep; const char* fmt; };
    constexpr Threshold kTable[] = {
        {1.0f,  "%.0f"},
        {0.1f,  "%.1f"},
        {0.01f, "%.2f"},
    };

    for (const auto& entry : kTable) {
        if (step >= entry.minStep) {
            return entry.fmt;
        }
    }
    return "%.3f";
}

} // namespace jst::tweaks