#pragma once

#include "tweaks/runtime_control.hpp"

#include <string_view>
#include <vector>

namespace jst::overlay {

void TextSv(std::string_view text);
void TextDisabledSv(std::string_view text);
[[nodiscard]] float ComputeLabelWidth(
    const std::vector<jst::tweaks::RuntimeControl>& controls);
[[nodiscard]] bool RenderControl(jst::tweaks::RuntimeControl& control, float labelWidth);
void ShowTooltipIfAny(std::string_view text);

} // namespace jst::overlay
