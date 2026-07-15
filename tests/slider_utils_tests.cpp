#include "tweaks/slider_specs.hpp"
#include "tweaks/slider_utils.hpp"
#include "tweaks/streaming_pool_policy.hpp"
#include "test_check.hpp"

#include <format>
#include <string_view>

void TestSliderUtils() {
    using jst::tweaks::DefaultSliderValue;
    using jst::tweaks::FloatSliderSpec;
    using jst::tweaks::LoadSliderValue;
    using jst::tweaks::NormalizeFloatSlider;
    using jst::tweaks::SliderDisplayFormat;
    using jst::tweaks::detail::SliderIndexToValue;
    using jst::tweaks::detail::SliderStepCount;
    using jst::tweaks::detail::SliderValueToIndex;

    constexpr FloatSliderSpec tenthStep{
        .min = 0.5f,
        .max = 1.5f,
        .defaultValue = 0.9f,
        .step = 0.1f,
    };
    Check(NearlyEqual(NormalizeFloatSlider(0.87f, tenthStep), 0.9f),
          "0.1 slider snaps 0.87 to 0.9");
    Check(NearlyEqual(NormalizeFloatSlider(-1.0f, tenthStep), 0.5f),
          "slider clamps below minimum");
    Check(NearlyEqual(NormalizeFloatSlider(2.0f, tenthStep), 1.5f),
          "slider clamps above maximum");

    constexpr FloatSliderSpec halfStep{
        .min = 1.0f,
        .max = 12.0f,
        .defaultValue = 2.0f,
        .step = 0.5f,
    };
    Check(NearlyEqual(NormalizeFloatSlider(2.3f, halfStep), 2.5f),
          "0.5 slider snaps 2.3 to 2.5");
    Check(NearlyEqual(NormalizeFloatSlider(12.0f, halfStep), 12.0f),
          "stepped slider retains exact maximum");

    constexpr FloatSliderSpec continuous{
        .min = 0.0f,
        .max = 10.0f,
        .defaultValue = 1.0f,
        .step = jst::tweaks::kFloatSliderStepContinuous,
    };
    Check(NearlyEqual(NormalizeFloatSlider(1.234f, continuous), 1.234f),
          "continuous slider preserves fractional input");
    Check(std::string_view(SliderDisplayFormat(0.5f)) == "%.1f",
          "0.5 step uses one decimal place");
    Check(std::string_view(SliderDisplayFormat(continuous.step)) == "%.3f",
          "continuous slider uses three decimal places");
    Check(NearlyEqual(LoadSliderValue(2.3f, halfStep), 2.5f),
          "loaded slider value clamps and snaps");
    Check(NearlyEqual(DefaultSliderValue(tenthStep), 0.9f),
          "configured slider default is normalized");

    constexpr FloatSliderSpec sharpening{
        .min = 0.0f,
        .max = 10.0f,
        .defaultValue = 1.0f,
        .step = 0.1f,
    };
    Check(SliderStepCount(sharpening) == 100,
          "0-10 slider exposes 100 tenth-steps");
    Check(NearlyEqual(NormalizeFloatSlider(9.999f, sharpening), 10.0f),
          "near-maximum input reaches declared maximum");
    Check(SliderValueToIndex(1.0f, tenthStep) == 5,
          "value 1.0 maps to tenth-step index 5");
    Check(NearlyEqual(SliderIndexToValue(5, tenthStep), 1.0f),
          "tenth-step index 5 maps back to 1.0");

    Check(NearlyEqual(
              NormalizeFloatSlider(
                  jst::tweaks::kAspectRatioSliderSpec.max,
                  jst::tweaks::kAspectRatioSliderSpec),
              1.5f),
          "aspect-ratio product spec retains its maximum");
    Check(NearlyEqual(
              NormalizeFloatSlider(
                  jst::tweaks::kSharpenSliderSpec.max,
                  jst::tweaks::kSharpenSliderSpec),
              10.0f),
          "sharpening product spec retains its maximum");
    Check(SliderStepCount(jst::tweaks::kMultiplierSliderSpec) == 100,
          "multiplier product spec exposes 100 steps");
    const auto pool24 = jst::tweaks::MakePoolSizePolicy(24ull << 30);
    Check(SliderStepCount(jst::tweaks::MakePoolSizeSliderSpec(pool24.limits)) == 163,
          "24 GiB pool spec exposes 0.5-16.8 in 0.1 steps");

    constexpr FloatSliderSpec awkward{
        .min = 0.0f,
        .max = 1.0f,
        .defaultValue = 0.0f,
        .step = 0.3f,
    };
    Check(SliderStepCount(awkward) == 3,
          "non-divisible span exposes a bounded step count");
    Check(NearlyEqual(SliderIndexToValue(3, awkward), 1.0f),
          "highest non-divisible index returns declared maximum");
    Check(NearlyEqual(NormalizeFloatSlider(0.99f, awkward), 1.0f),
          "non-divisible near-maximum snaps to maximum");

    constexpr FloatSliderSpec degenerate{
        .min = 5.0f,
        .max = 5.0f,
        .defaultValue = 5.0f,
        .step = 0.1f,
    };
    Check(SliderStepCount(degenerate) == 0,
          "zero-span slider exposes zero steps");
    Check(NearlyEqual(NormalizeFloatSlider(5.0f, degenerate), 5.0f),
          "degenerate slider preserves its only value");

    constexpr FloatSliderSpec fine{
        .min = 0.0f,
        .max = 0.05f,
        .defaultValue = 0.0f,
        .step = 0.01f,
    };
    Check(SliderStepCount(fine) == 5, "fine slider exposes five steps");
    Check(NearlyEqual(NormalizeFloatSlider(0.049f, fine), 0.05f),
          "fine near-maximum input reaches maximum");

    constexpr FloatSliderSpec roundTrip{
        .min = 0.0f,
        .max = 10.0f,
        .defaultValue = 5.0f,
        .step = 0.1f,
    };
    const int stepCount = SliderStepCount(roundTrip);
    for (int index = 0; index <= stepCount; ++index) {
        const float value = SliderIndexToValue(index, roundTrip);
        Check(SliderValueToIndex(value, roundTrip) == index,
              std::format("slider round-trip failed: {} -> {}", index, value));
        Check(NearlyEqual(NormalizeFloatSlider(value, roundTrip), value),
              std::format("slider boundary {} changed during normalization", index));
    }
    Check(stepCount + 1 == 101, "boundary walk covers all 101 slider values");
}
