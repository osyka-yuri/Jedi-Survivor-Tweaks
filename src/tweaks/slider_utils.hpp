#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

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

inline constexpr int kMaxSliderDecimalPlaces = 6;

[[nodiscard]] inline int SliderDecimalPlaces(float value) noexcept {
    if (!std::isfinite(value)) {
        return 0;
    }

    double scale = 1.0;
    for (int places = 0; places <= kMaxSliderDecimalPlaces; ++places) {
        const double scaled = static_cast<double>(value) * scale;
        const double tolerance =
            1e-7 * std::max(1.0, std::fabs(scaled));
        if (std::fabs(scaled - std::round(scaled)) <= tolerance) {
            return places;
        }
        scale *= 10.0;
    }
    return kMaxSliderDecimalPlaces;
}

[[nodiscard]] inline int64_t DecimalScale(int places) noexcept {
    int64_t scale = 1;
    for (int index = 0; index < places; ++index) {
        scale *= 10;
    }
    return scale;
}

struct SliderGrid {
    int64_t scale = 1;
    int64_t minUnits = 0;
    int64_t maxUnits = 0;
    int64_t stepUnits = 0;
};

[[nodiscard]] inline SliderGrid MakeSliderGrid(
    const FloatSliderSpec& spec) noexcept {
    const int places = std::max(
        SliderDecimalPlaces(spec.min),
        SliderDecimalPlaces(spec.step));
    const int64_t scale = DecimalScale(places);
    return SliderGrid{
        .scale = scale,
        .minUnits = std::llround(static_cast<double>(spec.min) * scale),
        .maxUnits = std::llround(static_cast<double>(spec.max) * scale),
        .stepUnits = std::llround(static_cast<double>(spec.step) * scale),
    };
}

[[nodiscard]] inline int SliderStepCount(const FloatSliderSpec& spec) noexcept {
    if (!HasSliderStep(spec.step)) {
        return 0;
    }

    const SliderGrid grid = MakeSliderGrid(spec);
    const int64_t spanUnits = grid.maxUnits - grid.minUnits;
    if (spanUnits <= 0 || grid.stepUnits <= 0) {
        return 0;
    }

    const int64_t count = std::llround(
        static_cast<double>(spanUnits) /
        static_cast<double>(grid.stepUnits));
    return static_cast<int>(std::clamp<int64_t>(
        count,
        0,
        std::numeric_limits<int>::max()));
}

[[nodiscard]] inline int ValueToStepIndex(
    double value,
    const SliderGrid& grid,
    int maxIndex) noexcept {
    if (grid.stepUnits <= 0) {
        return 0;
    }
    const double scaledValue = value * static_cast<double>(grid.scale);
    const double index = std::round(
        (scaledValue - static_cast<double>(grid.minUnits)) /
        static_cast<double>(grid.stepUnits));
    if (index <= 0.0) {
        return 0;
    }
    if (index >= static_cast<double>(maxIndex)) {
        return maxIndex;
    }
    return static_cast<int>(index);
}

[[nodiscard]] inline float StepIndexToValue(
    int index,
    const SliderGrid& grid,
    int maxIndex,
    float declaredMax) noexcept {
    if (index >= maxIndex) {
        return declaredMax;
    }
    if (index <= 0) {
        return static_cast<float>(
            static_cast<double>(grid.minUnits) /
            static_cast<double>(grid.scale));
    }
    const int64_t valueUnits =
        grid.minUnits + static_cast<int64_t>(index) * grid.stepUnits;
    return static_cast<float>(
        static_cast<double>(valueUnits) /
        static_cast<double>(grid.scale));
}

[[nodiscard]] inline float SliderIndexToValue(int index, const FloatSliderSpec& spec) noexcept {
    if (!HasSliderStep(spec.step)) {
        return std::clamp(static_cast<float>(index), spec.min, spec.max);
    }

    const int maxIndex = SliderStepCount(spec);
    const int clamped = std::clamp(index, 0, maxIndex);
    const SliderGrid grid = MakeSliderGrid(spec);
    return StepIndexToValue(
        clamped,
        grid,
        maxIndex,
        spec.max);
}

[[nodiscard]] inline int SliderValueToIndex(float value, const FloatSliderSpec& spec) noexcept {
    if (!HasSliderStep(spec.step)) {
        return 0;
    }

    const float clamped = std::clamp(value, spec.min, spec.max);
    const SliderGrid grid = MakeSliderGrid(spec);
    const int maxIndex = SliderStepCount(spec);

    if (clamped >= spec.max || SliderValuesNearlyEqual(clamped, spec.max)) {
        return maxIndex;
    }

    return ValueToStepIndex(static_cast<double>(clamped), grid, maxIndex);
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

    switch (detail::SliderDecimalPlaces(step)) {
    case 0: return "%.0f";
    case 1: return "%.1f";
    case 2: return "%.2f";
    case 3: return "%.3f";
    case 4: return "%.4f";
    case 5: return "%.5f";
    default: return "%.6f";
    }
}

} // namespace jst::tweaks
