#include "core/config.hpp"
#include "tweaks/runtime_control.hpp"
#include "tweaks/slider_utils.hpp"
#include "tweaks/tweak.hpp"
#include "test_check.hpp"

#include <expected>
#include <string>
#include <utility>

namespace {

class ResetTweak final : public jst::tweaks::ITweak {
public:
    explicit ResetTweak(jst::tweaks::RuntimeControlResetResult result)
        : m_result(result) {}

    std::string_view Name() const override { return "Test"; }
    std::string_view Description() const override { return "Test"; }
    std::expected<void, std::string> Initialize(
        jst::core::HookEngine&, jst::core::Config&) override {
        return {};
    }
    void Shutdown() override {}
    bool IsInitialized() const override { return true; }
    jst::tweaks::RuntimeControlResetResult ResetRuntimeControls(
        jst::core::Config&) override {
        ++resetCalls;
        return m_result;
    }

    int resetCalls = 0;

private:
    jst::tweaks::RuntimeControlResetResult m_result;
};

constexpr jst::tweaks::FloatSliderSpec kTenthStep{
    .min = 0.5f,
    .max = 1.5f,
    .defaultValue = 0.9f,
    .step = 0.1f,
};

} // namespace

void TestRuntimeControls() {
    using namespace jst::tweaks;

    {
        auto control = MakeSliderFloatControl(
            kTenthStep, 0.87f, [](float) {}, "test", "Section", "Value");
        Check(NearlyEqual(control.spec.defaultValue, 0.9f),
              "slider control normalizes its default");
        Check(NearlyEqual(control.current, 0.9f),
              "slider control normalizes its initial value");
    }

    {
        int applyCalls = 0;
        auto control = MakeSliderFloatControl(
            kTenthStep,
            0.5f,
            [&](float) { ++applyCalls; },
            "test",
            "Section",
            "Value");
        Check(TryCommitSliderEdit(control, 1.04f, control.current),
              "slider edit commits a changed grid value");
        Check(applyCalls == 1 && NearlyEqual(control.current, 1.0f),
              "slider edit normalizes and applies once");
        Check(!TryCommitSliderEdit(control, 1.00001f, control.current),
              "near-equal slider edit is unchanged");
        Check(applyCalls == 1, "unchanged slider edit does not apply again");
        RestoreSliderBaseline(control, 0.5f);
        Check(NearlyEqual(control.current, 0.5f),
              "idle frame can restore tweak-owned baseline");
    }

    // Ordinary persistence uses the control's typed config binding.
    {
        jst::core::Config config;
        RuntimeControl slider = MakeSliderFloatControl(
            kTenthStep, 1.2f, [](float) {}, "slider", "Runtime", "Scale");
        PersistControl(slider, config);
        Check(NearlyEqual(config.GetFloat("Runtime", "Scale", 0.0f), 1.2f),
              "slider persistence writes a float binding");

        RuntimeControl checkbox = CheckboxControl{
            .label = "checkbox",
            .current = true,
            .defaultValue = false,
            .apply = [](bool) {},
            .persistence = ControlPersistence{
                .section = "Runtime",
                .key = "Enabled",
            },
        };
        PersistControl(checkbox, config);
        Check(config.GetBool("Runtime", "Enabled", false),
              "checkbox persistence writes a bool binding");
    }

    // Override persistence owns the entire write and suppresses generic binding.
    {
        jst::core::Config config;
        int overrideCalls = 0;
        RuntimeControl control = MakeSliderFloatControl(
            kTenthStep, 1.1f, [](float) {}, "override", "Generic", "Value");
        auto& slider = std::get<SliderFloatControl>(control);
        slider.persistence.overrideAction = [&](jst::core::Config& target) {
            ++overrideCalls;
            target.SetString("Override", "Value", "custom");
        };
        PersistControl(control, config);
        Check(overrideCalls == 1, "persistence override is called exactly once");
        Check(config.GetString("Override", "Value", "") == "custom",
              "persistence override controls the config representation");
        Check(config.GetString("Generic", "Value", "missing") == "missing",
              "generic binding is skipped when override exists");
    }

    // Dynamic labels own their string instead of borrowing temporary storage.
    {
        std::string source = "dynamic status";
        RuntimeControl control = LabelControl{source};
        source.assign("overwritten");
        Check(std::get<LabelControl>(control).label == "dynamic status",
              "LabelControl owns dynamic status text");
    }

    // Unsupported delegates to generic controls; Unchanged and Changed are final.
    {
        jst::core::Config config;
        int applies = 0;
        std::vector<RuntimeControl> controls;
        controls.emplace_back(MakeSliderFloatControl(
            kTenthStep,
            1.5f,
            [&](float) { ++applies; },
            "slider",
            "Reset",
            "Value"));
        ResetTweak unsupported(RuntimeControlResetResult::Unsupported);
        Check(ResetTweakControls(unsupported, controls, config),
              "Unsupported reset delegates to generic control reset");
        Check(applies == 1 && NearlyEqual(
                  std::get<SliderFloatControl>(controls.front()).current, 0.9f),
              "generic reset applies the normalized default");
        Check(NearlyEqual(config.GetFloat("Reset", "Value", 0.0f), 0.9f),
              "generic reset persists its result");

        std::get<SliderFloatControl>(controls.front()).current = 1.5f;
        ResetTweak unchanged(RuntimeControlResetResult::Unchanged);
        Check(!ResetTweakControls(unchanged, controls, config),
              "Unchanged composite reset reports no mutation");
        Check(NearlyEqual(std::get<SliderFloatControl>(controls.front()).current, 1.5f),
              "Unchanged composite reset does not fall through to generic reset");

        ResetTweak changed(RuntimeControlResetResult::Changed);
        Check(ResetTweakControls(changed, controls, config),
              "Changed composite reset reports mutation");
        Check(unsupported.resetCalls == 1 && unchanged.resetCalls == 1 &&
                  changed.resetCalls == 1,
              "each typed reset result is evaluated exactly once");
    }

    {
        jst::core::Config config;
        RuntimeControl label = LabelControl{"read only"};
        Check(!ResetControlToDefault(label, config),
              "label has no resettable state");
    }
}
