#include "core/streaming_pool_protocol.hpp"
#include "tweaks/streaming_pool_controller.hpp"
#include "tweaks/streaming_pool_policy.hpp"
#include "test_check.hpp"

#include <atomic>
#include <cmath>
#include <limits>
#include <string>

namespace {

constexpr uint64_t GiB(uint64_t value) {
    return value * jst::tweaks::kPoolSizeBytesPerGiB;
}

uint64_t PublishedBytes(jst::core::StreamingPoolPayload& payload) {
    return std::atomic_ref<uint64_t>(payload.forcedBytes)
        .load(std::memory_order_acquire);
}

} // namespace

void TestStreamingPoolController() {
    using namespace jst::tweaks;

    {
        jst::core::StreamingPoolPayload payload;
        StreamingPoolController controller(MakePoolSizePolicy(GiB(2)));
        controller.BindPayload(payload);
        controller.ArmAuto(1);
        const auto exact = controller.CompleteAutoRead(1, 3000);
        Check(exact.completion == StreamingPoolAutoCompletion::Exact &&
                  exact.snapshot.state == StreamingPoolState::Automatic &&
                  exact.snapshot.enginePoolMb == 3000 &&
                  PublishedBytes(payload) ==
                      3000ull * kPoolSizeBytesPerMiB,
              "automatic mode publishes the exact 3000 MB engine value without GPU clamping");

        controller.ArmAuto(2);
        const auto nonGiB = controller.CompleteAutoRead(2, 3073);
        Check(nonGiB.completion == StreamingPoolAutoCompletion::Exact &&
                  PublishedBytes(payload) ==
                      3073ull * kPoolSizeBytesPerMiB,
              "automatic mode preserves every observed engine MiB exactly");

        Check(controller.UpdatePolicy(MakePoolSizePolicy(GiB(1))) &&
                  PublishedBytes(payload) ==
                      3073ull * kPoolSizeBytesPerMiB,
              "adapter policy changes never modify automatic values");
    }

    {
        jst::core::StreamingPoolPayload payload;
        StreamingPoolController controller(MakePoolSizePolicy(GiB(1)));
        controller.BindPayload(payload);
        controller.ArmAuto(10);
        const auto rejected = controller.CompleteAutoRead(
            10, std::unexpected(std::string("read rejected")));
        Check(rejected.completion == StreamingPoolAutoCompletion::Fallback &&
                  rejected.snapshot.state == StreamingPoolState::AutomaticFallback &&
                  PublishedBytes(payload) == 2ull * kPoolSizeBytesPerGiB &&
                  FormatStreamingPoolStatus(rejected.snapshot).find(
                      "Auto fallback: 2.00 GB") != std::string::npos,
              "a rejected automatic read converges once to the exact runtime-only 2 GiB fallback");

        controller.ArmAuto(11);
        const auto zero = controller.CompleteAutoRead(11, 0);
        controller.ArmAuto(12);
        const auto negative = controller.CompleteAutoRead(12, -1);
        Check(zero.completion == StreamingPoolAutoCompletion::Fallback &&
                  negative.completion == StreamingPoolAutoCompletion::Fallback &&
                  PublishedBytes(payload) == 2ull * kPoolSizeBytesPerGiB,
              "zero and negative automatic values use the same one-shot fallback");
    }

    {
        jst::core::StreamingPoolPayload payload;
        StreamingPoolController controller(MakePoolSizePolicy(GiB(24)));
        controller.BindPayload(payload);
        controller.ArmManual(20, 6.0f);
        controller.ArmAuto(21);
        Check(controller.Snapshot().state == StreamingPoolState::WaitingForEngine &&
                  PublishedBytes(payload) == GiB(6),
              "manual-to-auto retains its old forced value only while the read is pending");
        const auto exact = controller.CompleteAutoRead(21, 4096);
        Check(exact.completion == StreamingPoolAutoCompletion::Exact &&
                  PublishedBytes(payload) == GiB(4),
              "the current automatic completion replaces the temporary manual hold");

        controller.ArmAuto(22);
        controller.ArmManual(23, 3.0f);
        const auto stale = controller.CompleteAutoRead(22, 8192);
        Check(stale.completion == StreamingPoolAutoCompletion::Stale &&
                  controller.Snapshot().state == StreamingPoolState::Manual &&
                  PublishedBytes(payload) == GiB(3),
              "a stale automatic completion is a no-op");
    }

    {
        jst::core::StreamingPoolPayload payload;
        StreamingPoolController controller(MakePoolSizePolicy(GiB(8)));
        controller.BindPayload(payload);
        controller.ArmManual(30, 4.0f);
        Check(PublishedBytes(payload) == GiB(4),
              "manual values inside the active policy publish exactly");
        Check(controller.UpdatePolicy(MakePoolSizePolicy(GiB(2))) &&
                  PublishedBytes(payload) ==
                      14ull * kPoolSizeBytesPerGiB / 10,
              "only Manual mode is recomputed when the adapter policy changes");
        controller.ArmManual(31, std::numeric_limits<float>::quiet_NaN());
        Check(NearlyEqual(controller.Snapshot().effectiveGb, 1.4f),
              "non-finite manual input resolves through the manual policy only");
    }
}
