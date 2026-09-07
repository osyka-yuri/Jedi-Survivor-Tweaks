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
#include "core/logging.hpp"
#include "tweaks/runtime_control.hpp"
#include "tweaks/tweak.hpp"

#include <reshade/imgui_compat.hpp>
#include <reshade/reshade.hpp>

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
    auto& rc = app->GetConfig();

    static jst::core::DebounceTimer debounce(kSaveDebounceMs);
    bool anyChanged = false;

    tm.IterateTweaks([&anyChanged, &rc](
        jst::tweaks::ITweak& tw,
        const jst::tweaks::TweakStatus& status) {
        auto controls = tw.GetRuntimeControls();
        if (controls.empty()) return;

        const std::string_view nm = tw.Name();
        ImGui::PushID(nm.data(), nm.data() + nm.size());

        const auto runtimeStatus = tw.RuntimeStatus();
        const bool failed = status.stage == jst::tweaks::TweakStage::Failed ||
            (runtimeStatus && runtimeStatus->state ==
                jst::tweaks::TweakRuntimeState::Failed);
        const bool pending = runtimeStatus && runtimeStatus->state ==
            jst::tweaks::TweakRuntimeState::Pending;
        const bool inactive = runtimeStatus
            ? runtimeStatus->state == jst::tweaks::TweakRuntimeState::Inactive
            : status.stage == jst::tweaks::TweakStage::Disabled;
        const ImVec4 dotColor = failed
            ? ImVec4(0.85f, 0.25f, 0.25f, 1.0f)
            : pending
                ? ImVec4(0.95f, 0.70f, 0.20f, 1.0f)
                : inactive
                    ? ImVec4(0.55f, 0.55f, 0.55f, 1.0f)
                    : ImVec4(0.25f, 0.85f, 0.35f, 1.0f);

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

        const std::string_view statusMessage = !status.error.empty()
            ? std::string_view(status.error)
            : runtimeStatus && !runtimeStatus->message.empty()
                ? std::string_view(runtimeStatus->message)
                : std::string_view{};
        if (!statusMessage.empty()) {
            TextDisabledSv(statusMessage);
            ImGui::Spacing();
        }

        ImGui::Indent(kSectionIndent);
        const float labelWidth = overlay::ComputeLabelWidth(controls);
        int ctrlIdx = 0;
        for (auto& ctrl : controls) {
            ImGui::PushID(ctrlIdx++);
            const auto edit = overlay::RenderControl(ctrl, labelWidth);
            if (edit.ShouldPersist()) {
                PersistControl(ctrl, rc);
                anyChanged = true;
            } else if (edit.state ==
                       jst::tweaks::RuntimeEditState::Rejected) {
                JST_LOG_WARNING(
                    "Runtime edit for '{}' rejected: {}.",
                    tw.Name(),
                    edit.diagnostic);
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
        tm.IterateTweaks([&anyChanged, &rc](
            jst::tweaks::ITweak& tw,
            const jst::tweaks::TweakStatus&) {
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

    const auto& logger = jst::core::Logger::Instance();
    ImGui::PushTextWrapPos(0.0f);
    if (logger.HasFileSink()) {
        ImGui::TextDisabled("Log: %s", logger.GetLogPath().string().c_str());
    } else {
        ImGui::TextDisabled("Log: [In-memory only - log file could not be created]");
    }
    ImGui::PopTextWrapPos();

    if (ImGui::Button("Copy Log to Clipboard", ImVec2(0, 0))) {
        const auto dump = logger.DumpRecentEntries();
        ImGui::SetClipboardText(dump.c_str());
    }
}

} // namespace jst
