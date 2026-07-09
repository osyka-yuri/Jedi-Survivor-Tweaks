#include "tweaks/slider_specs.hpp"
#include "tweaks/slider_utils.hpp"

#include <cmath>
#include <format>
#include <iostream>
#include <string_view>

namespace {

void Check(bool condition, std::string_view message, int& failures) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr bool NearlyEqual(float lhs, float rhs) {
    return std::fabs(lhs - rhs) < 1e-5f;
}

} // namespace

int RunSliderUtilsTests() {
    int failures = 0;

    using jst::tweaks::DefaultSliderValue;
    using jst::tweaks::FloatSliderSpec;
    using jst::tweaks::LoadSliderValue;
    using jst::tweaks::NormalizeFloatSlider;
    using jst::tweaks::SliderDisplayFormat;
    using jst::tweaks::kFloatSliderStepContinuous;
    using jst::tweaks::detail::SliderIndexToValue;
    using jst::tweaks::detail::SliderStepCount;
    using jst::tweaks::detail::SliderValueToIndex;

    const FloatSliderSpec tenthStep{
        .min = 0.5f,
        .max = 1.5f,
        .defaultValue = 0.9f,
        .step = 0.1f,
    };
    Check(NearlyEqual(NormalizeFloatSlider(0.87f, tenthStep), 0.9f),
          "step 0.1 must snap 0.87 to 0.9", failures);

    const FloatSliderSpec halfStep{
        .min = 1.0f,
        .max = 12.0f,
        .defaultValue = 2.0f,
        .step = 0.5f,
    };
    Check(NearlyEqual(NormalizeFloatSlider(2.3f, halfStep), 2.5f),
          "step 0.5 must snap 2.3 to 2.5", failures);
    Check(NearlyEqual(NormalizeFloatSlider(12.0f, halfStep), 12.0f),
          "step 0.5 must keep max boundary value", failures);

    const FloatSliderSpec continuous{
        .min = 0.0f,
        .max = 10.0f,
        .defaultValue = 1.0f,
        .step = kFloatSliderStepContinuous,
    };
    Check(NearlyEqual(NormalizeFloatSlider(1.234f, continuous), 1.234f),
          "continuous slider must preserve fractional value", failures);

    Check(NearlyEqual(NormalizeFloatSlider(-1.0f, tenthStep), 0.5f),
          "value below min must clamp", failures);
    Check(NearlyEqual(NormalizeFloatSlider(2.0f, tenthStep), 1.5f),
          "value above max must clamp", failures);

    Check(std::string_view(SliderDisplayFormat(0.5f)) == "%.1f",
          "step 0.5 must use one decimal place", failures);
    Check(std::string_view(SliderDisplayFormat(kFloatSliderStepContinuous)) == "%.3f",
          "continuous slider must use three decimal places", failures);

    Check(NearlyEqual(LoadSliderValue(2.3f, halfStep), 2.5f),
          "LoadSliderValue must clamp and snap", failures);
    Check(NearlyEqual(DefaultSliderValue(tenthStep), 0.9f),
          "DefaultSliderValue must normalize configured default", failures);

    const FloatSliderSpec sharpening{
        .min = 0.0f,
        .max = 10.0f,
        .defaultValue = 1.0f,
        .step = 0.1f,
    };
    Check(SliderStepCount(sharpening) == 100,
          "0-10 with step 0.1 must expose 100 steps", failures);
    Check(NearlyEqual(NormalizeFloatSlider(9.999f, sharpening), 10.0f),
          "near-max drag must snap to 10.0, not 9.9", failures);
    Check(NearlyEqual(NormalizeFloatSlider(10.0f, sharpening), 10.0f),
          "max boundary must remain 10.0", failures);
    Check(SliderValueToIndex(1.0f, tenthStep) == 5,
          "aspect ratio 1.0 must map to step index 5", failures);
    Check(NearlyEqual(SliderIndexToValue(5, tenthStep), 1.0f),
          "aspect ratio step index 5 must map to 1.0", failures);

    // --- Product specs from slider_specs.hpp ---
    Check(NearlyEqual(NormalizeFloatSlider(jst::tweaks::kAspectRatioSliderSpec.max,
                                           jst::tweaks::kAspectRatioSliderSpec),
                      1.5f),
          "kAspectRatioSliderSpec max must normalize to 1.5", failures);
    Check(NearlyEqual(NormalizeFloatSlider(jst::tweaks::kSharpenSliderSpec.max,
                                           jst::tweaks::kSharpenSliderSpec),
                      10.0f),
          "kSharpenSliderSpec max must normalize to 10.0", failures);
    Check(SliderStepCount(jst::tweaks::kMultiplierSliderSpec) == 100,
          "kMultiplierSliderSpec must expose 100 steps", failures);
    Check(SliderStepCount(jst::tweaks::kPoolSizeSliderSpec) == 115,
          "kPoolSizeSliderSpec must expose 115 steps (0.5-12.0 by 0.1)", failures);

    // --- Extra thoroughness cases (non-divisible, near-max, degenerate) ---
    const FloatSliderSpec awkward{
        .min = 0.0f,
        .max = 1.0f,
        .defaultValue = 0.0f,
        .step = 0.3f,
    };
    Check(SliderStepCount(awkward) == 3,
          "non-divisible span must still produce a reasonable step count", failures);
    Check(NearlyEqual(SliderIndexToValue(3, awkward), 1.0f),
          "highest index for non-divisible spec must still return declared max", failures);
    Check(NearlyEqual(NormalizeFloatSlider(0.99f, awkward), 1.0f),
          "value very close to max must snap to declared max", failures);
    Check(NearlyEqual(NormalizeFloatSlider(1.0f, awkward), 1.0f),
          "exact max must remain max even for non-divisible step", failures);

    const FloatSliderSpec degenerate{
        .min = 5.0f,
        .max = 5.0f,
        .defaultValue = 5.0f,
        .step = 0.1f,
    };
    Check(SliderStepCount(degenerate) == 0,
          "zero-span spec must produce count 0 (or safe value)", failures);
    Check(NearlyEqual(NormalizeFloatSlider(5.0f, degenerate), 5.0f),
          "degenerate spec must at least preserve its only value", failures);

    const FloatSliderSpec tinyStep{
        .min = 0.0f,
        .max = 0.05f,
        .defaultValue = 0.0f,
        .step = 0.01f,
    };
    Check(SliderStepCount(tinyStep) == 5,
          "small range with 0.01 step must expose 5 steps", failures);
    Check(NearlyEqual(NormalizeFloatSlider(0.049f, tinyStep), 0.05f),
          "near-max with fine step must reach declared max", failures);

    // --- Round-trip fuzzing ---
    {
        const FloatSliderSpec spec{0.0f, 10.0f, 5.0f, 0.1f};
        const int n = SliderStepCount(spec);
        for (int i = 0; i <= n; ++i) {
            const float val = SliderIndexToValue(i, spec);
            const int j = SliderValueToIndex(val, spec);
            Check(j == i,
                  std::format("round-trip failure: index {} -> {} -> index {}", i, val, j), failures);
        }
    }

    // --- Boundary walk: all 101 values 0..10 with step 0.1 ---
    {
        const FloatSliderSpec spec{0.0f, 10.0f, 5.0f, 0.1f};
        int walked = 0;
        for (int i = 0; i <= SliderStepCount(spec); ++i) {
            const float value = SliderIndexToValue(i, spec);
            const float normalized = NormalizeFloatSlider(value, spec);
            Check(NearlyEqual(normalized, value),
                  std::format("boundary {}: {} normalized to {}", i, value, normalized), failures);
            ++walked;
        }
        Check(walked == 101, "boundary walk must cover all 101 steps", failures);
    }

    return failures;
}

extern int g_failures;

void TestSliderUtils() {
    g_failures += RunSliderUtilsTests();
}