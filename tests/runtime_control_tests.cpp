#include "tweaks/runtime_control.hpp"
#include "tweaks/slider_utils.hpp"

#include <iostream>
#include <string_view>

namespace {

void Check(bool condition, std::string_view message, int& failures) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

} // namespace

int RunRuntimeControlTests() {
    int failures = 0;

    using jst::tweaks::FloatSliderSpec;
    using jst::tweaks::MakeSliderFloatControl;
    using jst::tweaks::NormalizeFloatSlider;
    using jst::tweaks::RestoreSliderBaseline;
    using jst::tweaks::TryCommitSliderEdit;

    const FloatSliderSpec tenthStep{
        .min = 0.5f,
        .max = 1.5f,
        .defaultValue = 0.9f,
        .step = 0.1f,
    };

    // --- MakeSliderFloatControl: spec.defaultValue is normalized ---
    {
        auto ctrl = MakeSliderFloatControl(tenthStep, 0.0f, [](float) {}, "test", "", "");
        const float expectedDefault = NormalizeFloatSlider(0.9f, tenthStep);
        Check(ctrl.spec.defaultValue == expectedDefault,
              "MakeSliderFloatControl must normalize spec.defaultValue", failures);
    }

    // --- MakeSliderFloatControl: current is normalized on construction ---
    {
        auto ctrl = MakeSliderFloatControl(tenthStep, 0.87f, [](float) {}, "test", "", "");
        Check(ctrl.current == 0.9f,
              "MakeSliderFloatControl must normalize current on construction", failures);
    }

    // --- TryCommitSliderEdit: apply called when value changes ---
    {
        int applyCalls = 0;
        auto ctrl = MakeSliderFloatControl(tenthStep, 0.5f,
            [&applyCalls](float) { ++applyCalls; }, "test", "", "");

        const bool changed = TryCommitSliderEdit(ctrl, 1.0f, ctrl.current);
        Check(changed, "TryCommitSliderEdit must return true when value changed", failures);
        Check(applyCalls == 1, "TryCommitSliderEdit must call apply when value changed", failures);
        Check(ctrl.current == 1.0f,
              "TryCommitSliderEdit must update control.current after normalize", failures);
    }

    // --- TryCommitSliderEdit: apply NOT called when near-equal ---
    {
        int applyCalls = 0;
        auto ctrl = MakeSliderFloatControl(tenthStep, 0.9f,
            [&applyCalls](float) { ++applyCalls; }, "test", "", "");

        const bool changed = TryCommitSliderEdit(ctrl, 0.90001f, 0.9f);
        Check(!changed, "TryCommitSliderEdit must return false when value is near-equal", failures);
        Check(applyCalls == 0,
              "TryCommitSliderEdit must NOT call apply when value is near-equal", failures);
    }

    // --- TryCommitSliderEdit: persistedBaseline triggers apply even for same grid point ---
    {
        int applyCalls = 0;
        auto ctrl = MakeSliderFloatControl(tenthStep, 0.5f,
            [&applyCalls](float) { ++applyCalls; }, "test", "", "");
        ctrl.current = 0.9f;

        const bool changed = TryCommitSliderEdit(ctrl, 0.9f, 0.5f);
        Check(changed, "TryCommitSliderEdit must detect stale persistedBaseline", failures);
        Check(applyCalls == 1,
              "TryCommitSliderEdit must call apply when persistedBaseline differs from normalized", failures);
    }

    // --- Reset via TryCommitSliderEdit(spec.defaultValue) ---
    {
        int applyCalls = 0;
        auto ctrl = MakeSliderFloatControl(tenthStep, 0.0f,
            [&applyCalls](float) { ++applyCalls; }, "test", "", "");
        ctrl.current = 1.5f;

        const bool changed = TryCommitSliderEdit(ctrl, ctrl.spec.defaultValue, ctrl.current);
        Check(changed, "reset via TryCommitSliderEdit must return true", failures);
        Check(applyCalls == 1, "reset via TryCommitSliderEdit must call apply", failures);
    }

    // --- RestoreSliderBaseline: idle frame restores tweak-owned value ---
    {
        auto ctrl = MakeSliderFloatControl(tenthStep, 0.5f, [](float) {}, "test", "", "");
        ctrl.current = 1.0f;

        RestoreSliderBaseline(ctrl, 0.5f);
        Check(ctrl.current == 0.5f,
              "RestoreSliderBaseline must restore persisted tweak value", failures);
    }

    return failures;
}

extern int g_failures;

void TestRuntimeControls() {
    g_failures += RunRuntimeControlTests();
}