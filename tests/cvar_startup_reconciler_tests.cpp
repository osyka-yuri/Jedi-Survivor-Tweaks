#include "core/cvar_startup_reconciler.hpp"
#include "test_check.hpp"

#include <chrono>

void TestCVarStartupReconciler() {
    using namespace std::chrono_literals;
    using jst::core::CVarStartupReconciler;
    using jst::core::CVarValueKind;

    const auto start = std::chrono::steady_clock::time_point{10s};
    CVarStartupReconciler reconciler;

    reconciler.Track(
        L"r.Test.Value", L"1", CVarValueKind::Integer, 1);
    reconciler.Track(
        L"R.TEST.VALUE", L"2", CVarValueKind::Integer, 2);
    Check(reconciler.Start(start, 15s, 100ms) == 1,
          "startup reconciler coalesces names case-insensitively");

    reconciler.CaptureApplied(L"r.test.value", 1, 1);
    Check(reconciler.Evaluate(start + 100ms).targets.empty(),
          "superseded generations cannot publish an expected value");

    reconciler.CaptureApplied(L"r.test.value", 2, 2);
    Check(reconciler.Evaluate(start + 150ms).targets.empty(),
          "startup reconciliation respects its fixed evaluation interval");

    const auto due = reconciler.Evaluate(start + 200ms);
    Check(due.targets.size() == 1 &&
              jst::core::CVarNameEqual{}(
                  due.targets.front().name, L"R.TEST.VALUE") &&
              due.targets.front().value == L"2" &&
              due.targets.front().generation == 2 &&
              due.targets.front().expectedValueWord == 2,
          "due evaluation returns the latest applied target exactly once");

    Check(reconciler.TryBeginCorrection(
              L"r.test.value", 2, due.targets.front().permit) &&
              reconciler.RecordCorrection(
                  L"r.test.value", 2, due.targets.front().permit, 3),
          "current generation accepts a corrected observed value");
    const auto corrected = reconciler.Evaluate(start + 300ms);
    Check(corrected.targets.size() == 1 &&
              corrected.targets.front().expectedValueWord == 3,
          "successful correction becomes the next comparison baseline");

    const auto finished = reconciler.Evaluate(start + 15s);
    Check(finished.finishedCorrectionCount == 1 &&
              finished.targets.empty(),
          "deadline ends the window and reports its correction count");

    reconciler.Track(
        L"r.Runtime.Value", L"4", CVarValueKind::Integer, 3);
    Check(reconciler.Evaluate(start + 17s).targets.empty(),
          "runtime writes after the startup window are not monitored");

    reconciler.Clear();
    reconciler.Track(
        L"r.Next.Start", L"5", CVarValueKind::Integer, 4);
    Check(reconciler.Start(start + 20s, 15s, 100ms) == 1,
          "clear resets the reconciler for a new process lifecycle");
    reconciler.Cancel(L"R.NEXT.START");
    Check(reconciler.Evaluate(start + 20100ms).targets.empty(),
          "specialized ownership cancels a target case-insensitively");

    reconciler.Clear();
    reconciler.Track(
        L"r.Generation", L"6", CVarValueKind::Integer, 6);
    Check(!reconciler.Cancel(L"r.Generation", 5) &&
              reconciler.Cancel(L"R.GENERATION", 6),
          "generation-aware cancellation cannot remove a newer target");
}
