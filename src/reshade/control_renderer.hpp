#pragma once

#include "core/config.hpp"
#include "tweaks/runtime_control.hpp"

#include <string_view>
#include <vector>

namespace jst::overlay {

void TextSv(std::string_view sv);
void TextDisabledSv(std::string_view sv);

[[nodiscard]] float ComputeLabelWidth(const std::vector<jst::tweaks::RuntimeControl>& controls);

[[nodiscard]] bool RenderControl(jst::tweaks::RuntimeControl& ctrl, float labelWidth);

void PersistControl(const jst::tweaks::RuntimeControl& ctrl, jst::core::Config& rc);

// Reset a control to its default value, call apply, and write to config.
// Returns true if the control actually changed.
[[nodiscard]] bool ResetControlToDefault(jst::tweaks::RuntimeControl& ctrl, jst::core::Config& rc);

void ShowTooltipIfAny(std::string_view text);

} // namespace jst::overlay