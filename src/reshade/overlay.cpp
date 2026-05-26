// ReShade ImGui overlay for JediSurvivorTweaks.
// Compiled into the `ReleaseAddon|x64` configuration only (excluded from
// Release/Debug) via the <ExcludedFromBuild> pattern in the vcxproj.
//
// Rendering model:
//   - DrawOverlay is called every render frame while the ReShade UI is open.
//   - Each frame: IterateTweaks() -> GetRuntimeControls() per tweak -> render.
//   - Slider/checkbox changes call `apply(newValue)` immediately (zero-latency
//     runtime effect) and update the snapshot stored in the control struct.
//   - Changes are persisted to Config (SaveMode::Deterministic) with a
//     debounced autosave rather than immediate disk writes.

#include "overlay.hpp"
#include "main_app.hpp"
#include "core/config.hpp"
#include "tweaks/runtime_control.hpp"
#include "tweaks/tweak.hpp"

#pragma warning(push)
#pragma warning(disable: 4100)  // unreferenced formal parameter
#pragma warning(disable: 4127)  // conditional expression is constant
#pragma warning(disable: 4324)  // structure padded due to alignment
#include <external/reshade/imgui_compat.hpp>   // must precede reshade.hpp to unlock ImGui:: wrappers
#include <external/reshade/reshade.hpp>
#pragma warning(pop)

#include <algorithm>
#include <variant>
#include <windows.h>

namespace jst {

namespace {

    // Layout / debounce constants. Grouped here so future tuning is a single
    // diff hunk rather than a hunt across the file.
    constexpr float     kTooltipWrapInFontUnits = 35.0f;  // ImGui::GetFontSize() multiplier
    constexpr float     kLabelColumnRatio       = 0.45f;  // fraction of avail. region for label column
    constexpr float     kLabelColumnMin         = 100.0f;
    constexpr float     kLabelColumnMax         = 200.0f;  // soft cap; long labels lift it
    constexpr float     kLabelColumnGapPx       = 12.0f;  // gap between label and control
    constexpr float     kResetButtonWidth       = 55.0f;
    constexpr float     kSectionIndent          = 10.0f;
    constexpr float     kControlRightPad        = 10.0f;  // gap reserved after the control column
    constexpr float     kControlMinWidth        = 30.0f;
    constexpr ULONGLONG kSaveDebounceMs         = 500;

    // Render a string_view without requiring null-termination. ImGui's
    // TextUnformatted takes [begin, end) pointers, perfect for the slice
    // we get from a string_view.
    inline void TextSv(std::string_view sv) {
        ImGui::TextUnformatted(sv.data(), sv.data() + sv.size());
    }

    // The imgui_compat.hpp shim doesn't expose ImGuiCol_* constants, so we
    // can't push the TextDisabled colour around TextUnformatted ourselves.
    // ImGui::TextDisabled with %.*s avoids the issue and respects the
    // current style.
    inline void TextDisabledSv(std::string_view sv) {
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

    // Responsive label width with longest-slider-label floor. Only sliders
    // need the column to be wide enough -- checkboxes are right-aligned and
    // ignore labelWidth entirely. The responsive ratio is clamped to a soft
    // range, then lifted to at least the widest slider label + a gap so the
    // slider never sits on top of its label.
    float ComputeLabelWidth(const std::vector<jst::tweaks::RuntimeControl>& controls) {
        const float responsive = std::clamp(
            ImGui::GetContentRegionAvail().x * kLabelColumnRatio,
            kLabelColumnMin, kLabelColumnMax);

        float widestSliderLabel = 0.0f;
        for (const auto& ctrl : controls) {
            std::visit([&widestSliderLabel](const auto& c) {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, jst::tweaks::SliderFloatControl>) {
                    // (text, text_end, hide_after_##=false, wrap_width=-1)
                    const ImVec2 sz = ImGui::CalcTextSize(c.label.data(),
                                                          c.label.data() + c.label.size(),
                                                          false, -1.0f);
                    if (sz.x > widestSliderLabel) widestSliderLabel = sz.x;
                }
            }, ctrl);
        }
        return std::max(responsive, widestSliderLabel + kLabelColumnGapPx);
    }

    // Render one RuntimeControl widget.  Returns true if the value changed.
    bool RenderControl(jst::tweaks::RuntimeControl& ctrl, float labelWidth) {
        return std::visit([labelWidth](auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, jst::tweaks::SliderFloatControl>) {
                ImGui::AlignTextToFramePadding();
                TextSv(c.label);
                ShowTooltipIfAny(c.tooltip);

                // Position the slider's left edge at max(precomputed column,
                // actual_label_end + gap). Reading the real end position via
                // SameLine(0,0) + GetCursorPosX is bulletproof: even when the
                // CalcTextSize-based labelWidth column underestimates the real
                // pixel width (font kerning, ItemSpacing quirks), the per-row
                // fallback guarantees the slider can't overlap its label.
                ImGui::SameLine(0.0f, 0.0f);
                const float labelEndX = ImGui::GetCursorPosX();
                const float columnX   = ImGui::GetCursorStartPos().x + labelWidth;
                ImGui::SetCursorPosX(std::max(columnX, labelEndX + kLabelColumnGapPx));

                const float avail = ImGui::GetContentRegionAvail().x;
                const float controlW = std::max(avail - kControlRightPad, kControlMinWidth);
                ImGui::PushItemWidth(controlW);
                const bool changed = ImGui::SliderFloat("##slider", &c.current, c.min, c.max, "%.3f", 0);
                ImGui::PopItemWidth();
                if (changed) c.apply(c.current);
                ShowTooltipIfAny(c.tooltip);
                return changed;
            } else if constexpr (std::is_same_v<T, jst::tweaks::CheckboxControl>) {
                // Labels are string literals in this codebase, so data() is
                // null-terminated and safe to pass to ImGui::Checkbox.
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

    struct SaveState {
        bool      pending = false;
        ULONGLONG lastChangeTick = 0;
    };

    // Write a single control's current value into the runtime config.
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

    // Reset a single control to its default value, call apply, and write to
    // the runtime config. Returns true if the control actually changed.
    bool ResetControlToDefault(jst::tweaks::RuntimeControl& ctrl, jst::core::Config& rc) {
        return std::visit([&](auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, jst::tweaks::SliderFloatControl>) {
                if (c.current == c.defaultValue) return false;
                c.current = c.defaultValue;
                c.apply(c.current);
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

} // anonymous namespace

void DrawOverlay(reshade::api::effect_runtime* /*runtime*/) {
    auto* app = jst::GetRunningApplication();
    if (!app) {
        ImGui::TextUnformatted("Bootstrapping -- please wait...", nullptr);
        return;
    }

    auto& tm = app->GetTweakManager();
    auto& rc = app->GetConfigMutable();

    // ── Per-tweak sections ─────────────────────────────────────────────────
    static SaveState saveState;
    bool anyChanged = false;

    tm.IterateTweaks([&anyChanged, &rc](jst::tweaks::ITweak& tw) {
        auto controls = tw.GetRuntimeControls();
        if (controls.empty()) return;

        // Use the [begin, end) overload so we don't rely on Name()'s
        // string_view storage being null-terminated.
        const std::string_view nm = tw.Name();
        ImGui::PushID(nm.data(), nm.data() + nm.size());

        // Header line: name + status dot + per-tweak Reset button
        const bool initOk = tw.IsInitialized();
        const ImVec4 dotColor = initOk ? ImVec4(0.25f, 0.85f, 0.35f, 1.0f)
                                       : ImVec4(0.85f, 0.25f, 0.25f, 1.0f);

        TextSv(tw.Name());
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(dotColor, "●");

        // Right-align mini "Reset" button
        {
            const float xPos = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - kResetButtonWidth;
            ImGui::SameLine(xPos, 0.0f);
            if (ImGui::Button("Reset", ImVec2(kResetButtonWidth, 0))) {
                for (auto& ctrl : controls) {
                    if (ResetControlToDefault(ctrl, rc))
                        anyChanged = true;
                }
            }
        }

        ImGui::Separator();

        // Description (muted, directly below header). Wrapped to the panel
        // width so longer descriptions break across lines instead of running
        // off the right edge of the panel.
        if (!tw.Description().empty()) {
            ImGui::PushTextWrapPos(0.0f);  // 0 = wrap to current window's right edge
            TextDisabledSv(tw.Description());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }

        ImGui::Indent(kSectionIndent);
        const float labelWidth = ComputeLabelWidth(controls);
        int ctrlIdx = 0;
        for (auto& ctrl : controls) {
            ImGui::PushID(ctrlIdx++);
            if (RenderControl(ctrl, labelWidth)) {
                PersistControl(ctrl, rc);
                anyChanged = true;
            }
            ImGui::PopID();
            ImGui::Spacing();
        }
        ImGui::Unindent(kSectionIndent);

        ImGui::PopID();
        ImGui::Spacing();
    });

    if (anyChanged) {
        saveState.pending = true;
        saveState.lastChangeTick = GetTickCount64();
    }

    if (saveState.pending && GetTickCount64() - saveState.lastChangeTick >= kSaveDebounceMs) {
        (void)rc.Save();
        saveState.pending = false;
    }

    // ── Global Reset & Footer ──────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Reset to Defaults", ImVec2(0, 0))) {
        tm.IterateTweaks([&anyChanged, &rc](jst::tweaks::ITweak& tw) {
            auto controls = tw.GetRuntimeControls();
            for (auto& ctrl : controls) {
                if (ResetControlToDefault(ctrl, rc))
                    anyChanged = true;
            }
        });
    }

    if (!rc.GetPath().empty()) {
        ImGui::Spacing();
        // Wrap long paths the same way as tweak descriptions so deep install
        // locations don't run off the right edge of the panel.
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Config: %s", rc.GetPath().string().c_str());
        ImGui::PopTextWrapPos();
    }
}

} // namespace jst
