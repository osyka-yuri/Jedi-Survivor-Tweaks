// ImGui rendering helpers for RuntimeControl widgets.
// Compiled into `ReleaseAddon|x64` only (ExcludedFromBuild for Release/Debug).

#include "control_renderer.hpp"

#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4127)
#pragma warning(disable: 4324)
#include <external/reshade/imgui_compat.hpp>
#include <external/reshade/reshade.hpp>
#pragma warning(pop)

#include <algorithm>

namespace jst::overlay {

namespace {

    constexpr float kTooltipWrapInFontUnits = 35.0f;
    constexpr float kLabelColumnRatio       = 0.45f;
    constexpr float kLabelColumnMin         = 100.0f;
    constexpr float kLabelColumnMax         = 200.0f;
    constexpr float kLabelColumnGapPx       = 12.0f;
    constexpr float kControlRightPad        = 10.0f;
    constexpr float kControlMinWidth        = 30.0f;

    void AlignSliderLabel(jst::tweaks::SliderFloatControl& c, float labelWidth) {
        ImGui::AlignTextToFramePadding();
        TextSv(c.label);
        ShowTooltipIfAny(c.tooltip);

        ImGui::SameLine(0.0f, 0.0f);
        const float labelEndX = ImGui::GetCursorPosX();
        const float columnX   = ImGui::GetCursorStartPos().x + labelWidth;
        ImGui::SetCursorPosX(std::max(columnX, labelEndX + kLabelColumnGapPx));
    }

    [[nodiscard]] bool RenderSliderFloatControl(
        jst::tweaks::SliderFloatControl& c, float labelWidth) {
        AlignSliderLabel(c, labelWidth);

        const float avail = ImGui::GetContentRegionAvail().x;
        const float controlW = std::max(avail - kControlRightPad, kControlMinWidth);
        ImGui::PushItemWidth(controlW);

        const float persisted = c.current;
        const ImGuiSliderFlags sliderFlags =
            jst::tweaks::HasSliderStep(c.spec.step)
                ? ImGuiSliderFlags_NoRoundToFormat
                : ImGuiSliderFlags_None;
        ImGui::SliderFloat(
            "##slider",
            &c.current,
            c.spec.min,
            c.spec.max,
            jst::tweaks::SliderDisplayFormat(c.spec.step),
            sliderFlags);
        ImGui::PopItemWidth();

        bool changed = false;
        if (ImGui::IsItemActive() || ImGui::IsItemEdited() || ImGui::IsItemDeactivatedAfterEdit()) {
            changed = jst::tweaks::TryCommitSliderEdit(c, c.current, persisted);
        } else {
            jst::tweaks::RestoreSliderBaseline(c, persisted);
        }

        ShowTooltipIfAny(c.tooltip);
        return changed;
    }

} // namespace

void TextSv(std::string_view sv) {
    ImGui::TextUnformatted(sv.data(), sv.data() + sv.size());
}

void TextDisabledSv(std::string_view sv) {
    ImGui::TextDisabled("%.*s", static_cast<int>(sv.size()), sv.data());
}

void ShowTooltipIfAny(std::string_view text) {
    if (text.empty() || !ImGui::IsItemHovered(0)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * kTooltipWrapInFontUnits);
    TextSv(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

float ComputeLabelWidth(const std::vector<jst::tweaks::RuntimeControl>& controls) {
    const float responsive = std::clamp(
        ImGui::GetContentRegionAvail().x * kLabelColumnRatio,
        kLabelColumnMin, kLabelColumnMax);

    float widestSliderLabel = 0.0f;
    for (const auto& ctrl : controls) {
        std::visit([&widestSliderLabel](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, jst::tweaks::SliderFloatControl>) {
                const ImVec2 sz = ImGui::CalcTextSize(c.label.data(),
                                                       c.label.data() + c.label.size(),
                                                       false, -1.0f);
                if (sz.x > widestSliderLabel) widestSliderLabel = sz.x;
            }
        }, ctrl);
    }
    return std::max(responsive, widestSliderLabel + kLabelColumnGapPx);
}

bool RenderControl(jst::tweaks::RuntimeControl& ctrl, float labelWidth) {
    return std::visit([labelWidth](auto& c) -> bool {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, jst::tweaks::SliderFloatControl>) {
            return RenderSliderFloatControl(c, labelWidth);
        } else if constexpr (std::is_same_v<T, jst::tweaks::CheckboxControl>) {
            const bool changed = ImGui::Checkbox(c.label.data(), &c.current);
            if (changed) c.apply(c.current);
            ShowTooltipIfAny(c.tooltip);
            return changed;
        } else {
            TextDisabledSv(c.label);
            return false;
        }
    }, ctrl);
}

void PersistControl(const jst::tweaks::RuntimeControl& ctrl, jst::core::Config& rc) {
    std::visit([&rc](auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, jst::tweaks::SliderFloatControl>) {
            if (!c.configSection.empty())
                rc.SetFloat(c.configSection, c.configKey, c.current);
        } else if constexpr (std::is_same_v<T, jst::tweaks::CheckboxControl>) {
            if (!c.configSection.empty())
                rc.SetBool(c.configSection, c.configKey, c.current);
        }
    }, ctrl);
}

bool ResetControlToDefault(jst::tweaks::RuntimeControl& ctrl, jst::core::Config& rc) {
    return std::visit([&](auto& c) -> bool {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, jst::tweaks::SliderFloatControl>) {
            if (!jst::tweaks::TryCommitSliderEdit(c, c.spec.defaultValue, c.current)) {
                return false;
            }
            PersistControl(ctrl, rc);
            return true;
        } else if constexpr (std::is_same_v<T, jst::tweaks::CheckboxControl>) {
            if (c.current == c.defaultValue) return false;
            c.current = c.defaultValue;
            c.apply(c.current);
            PersistControl(ctrl, rc);
            return true;
        } else {
            return false;
        }
    }, ctrl);
}

} // namespace jst::overlay