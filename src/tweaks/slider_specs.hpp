#pragma once

#include "slider_utils.hpp"

namespace jst::tweaks {

inline constexpr float kSliderStepTenth = 0.1f;

inline constexpr FloatSliderSpec kMultiplierSliderSpec{
    .min = 0.0f,
    .max = 10.0f,
    .defaultValue = 1.0f,
    .step = kSliderStepTenth,
};

inline constexpr FloatSliderSpec kAspectRatioSliderSpec{
    .min = 0.5f,
    .max = 1.5f,
    .defaultValue = 0.9f,
    .step = kSliderStepTenth,
};

inline constexpr FloatSliderSpec kSharpenSliderSpec{
    .min = 0.0f,
    .max = 10.0f,
    .defaultValue = 1.0f,
    .step = kSliderStepTenth,
};

static_assert(IsValidSpec(kMultiplierSliderSpec));
static_assert(IsValidSpec(kAspectRatioSliderSpec));
static_assert(IsValidSpec(kSharpenSliderSpec));

} // namespace jst::tweaks
