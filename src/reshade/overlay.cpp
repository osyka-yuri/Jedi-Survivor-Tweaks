// ReShade ImGui overlay for JediSurvivorTweaks.
// Compiled into the `ReleaseAddon|x64` configuration only (excluded from
// Release/Debug) via the <ExcludedFromBuild> pattern in the vcxproj.
//
// DrawOverlay is called every render frame while the ReShade UI is open.
// Per-tweak iteration -> GetRuntimeControls() -> render via ControlRenderer.
// Changes are auto-saved to Config after a debounce.

#include "overlay.hpp"
#include "control_renderer.hpp"
#include "main_app.hpp"
#include "core/config.hpp"
#include "core/debounce_timer.hpp"
#include "tweaks/runtime_control.hpp"
#include "tweaks/tweak.hpp"

#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4127)
#pragma warning(disable: 4324)
#include <external/reshade/imgui_compat.hpp>
#include <external/reshade/reshade.hpp>
#pragma warning(pop)

#include <algorithm>

namespace jst {

namespace {

    constexpr float kResetButtonWidth = 55.0f;
    constexpr float kSectionIndent    = 10.0f;
    constexpr ULONGLONG kSaveDebounceMs = 500;

using jst::overlay::ComputeLabelWidth;
using jst::overlay::RenderControl;
using jst::overlay::TextDisabledSv;
using jst::overlay::TextSv;
using jst::tweaks::PersistControl;
using jst::tweaks::ResetTweakControls;

} // namespace

void DrawOverlay(::reshade::api::effect_runtime* /*runtime*/) {
    auto* app = jst::GetRunningApplication();
    if (!app) {
        ImGui::TextUnformatted("Bootstrapping -- please wait...", nullptr);
        return;
    }

    auto& tm = app->GetTweakManager();
    auto& rc = app->GetConfigMutable();

    static jst::core::DebounceTimer debounce(kSaveDebounceMs);
    bool anyChanged = false;

    tm.IterateTweaks([&anyChanged, &rc](jst::tweaks::ITweak& tw) {
        auto controls = tw.GetRuntimeControls();
        if (controls.empty()) return;

        const std::string_view nm = tw.Name();
        ImGui::PushID(nm.data(), nm.data() + nm.size());

        const bool initOk = tw.IsInitialized();
        const ImVec4 dotColor = initOk ? ImVec4(0.25f, 0.85f, 0.35f, 1.0f)
                                       : ImVec4(0.85f, 0.25f, 0.25f, 1.0f);

        TextSv(tw.Name());
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(dotColor, "\xe2\x97\x8f");

        {
            const float xPos = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - kResetButtonWidth;
            ImGui::SameLine(xPos, 0.0f);
            if (ImGui::Button("Reset", ImVec2(kResetButtonWidth, 0))) {
                anyChanged = ResetTweakControls(tw, controls, rc) || anyChanged;
            }
        }

        ImGui::Separator();

        if (!tw.Description().empty()) {
            ImGui::PushTextWrapPos(0.0f);
            TextDisabledSv(tw.Description());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }

        ImGui::Indent(kSectionIndent);
        const float labelWidth = overlay::ComputeLabelWidth(controls);
        int ctrlIdx = 0;
        for (auto& ctrl : controls) {
            ImGui::PushID(ctrlIdx++);
            if (overlay::RenderControl(ctrl, labelWidth)) {
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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Reset to Defaults", ImVec2(0, 0))) {
        tm.IterateTweaks([&anyChanged, &rc](jst::tweaks::ITweak& tw) {
            auto controls = tw.GetRuntimeControls();
            anyChanged = ResetTweakControls(tw, controls, rc) || anyChanged;
        });
    }

    if (anyChanged) {
        debounce.MarkDirty();
    }

    if (debounce.ShouldFlush()) {
        (void)rc.Save();
        debounce.Reset();
    }

    if (!rc.GetPath().empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Config: %s", rc.GetPath().string().c_str());
        ImGui::PopTextWrapPos();
    }
}

} // namespace jst
