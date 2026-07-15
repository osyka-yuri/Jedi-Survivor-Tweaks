#include "core/streaming_pool_protocol.hpp"
#include "tweaks/streaming_pool_controller.hpp"
#include "tweaks/streaming_pool_policy.hpp"
#include "test_check.hpp"

#include <cmath>
#include <limits>
#include <string>

namespace {

constexpr uint64_t GiB(uint64_t value) {
    return value * jst::core::kBytesPerGiB;
}

} // namespace

void TestPoolSizeSetting() {
    using namespace jst::tweaks;

    // Dedicated VRAM produces an exact 70% ceiling rounded down to the UI grid.
    {
        const auto p24 = MakePoolSizePolicy(GiB(24));
        const auto p8 = MakePoolSizePolicy(GiB(8));
        const auto p2 = MakePoolSizePolicy(GiB(2));
        const auto fractional = MakePoolSizePolicy(GiB(7) + GiB(1) / 2);
        const auto legacy = MakePoolSizePolicy(std::nullopt);
        Check(p24.HasDedicatedVideoMemory(), "24 GiB dedicated VRAM is retained");
        Check(NearlyEqual(p24.limits.MaximumGb(), 16.8f), "24 GiB -> 16.8 GiB maximum");
        Check(NearlyEqual(p24.limits.FallbackGb(), 2.0f), "24 GiB fallback remains 2 GiB");
        Check(NearlyEqual(p8.limits.MaximumGb(), 5.6f), "8 GiB -> 5.6 GiB maximum");
        Check(NearlyEqual(p2.limits.MaximumGb(), 1.4f), "2 GiB -> 1.4 GiB maximum");
        Check(NearlyEqual(p2.limits.FallbackGb(), 1.4f), "2 GiB fallback is policy-capped");
        Check(NearlyEqual(fractional.limits.MaximumGb(), 5.2f),
              "fractional VRAM is floored to the 0.1 GiB policy grid");
        Check(fractional.limits.maximumBytes ==
                  52ull * jst::core::kBytesPerGiB / 10,
              "fractional policy ceiling uses exact rational GiB bytes");
        Check(fractional.limits.maximumBytes <=
                  *fractional.dedicatedVideoMemoryBytes * 7 / 10,
              "fractional ceiling never exceeds 70% dedicated VRAM");
        Check(!legacy.HasDedicatedVideoMemory(), "unknown VRAM uses legacy policy");
        Check(NearlyEqual(legacy.limits.MaximumGb(), 12.0f),
              "legacy ceiling remains 12 GiB");
    }

    // Even very small fixed VRAM cannot make the policy internally impossible.
    {
        const auto tiny = MakePoolSizePolicy(256ull * jst::core::kBytesPerMiB);
        Check(NearlyEqual(tiny.limits.MinimumGb(), 0.5f) &&
                  NearlyEqual(tiny.limits.MaximumGb(), 0.5f) &&
                  NearlyEqual(tiny.limits.FallbackGb(), 0.5f),
              "minimum ceiling and fallback converge at 0.5 GiB");
    }

    Check(IsPoolSizeAutoLiteral("auto"), "auto literal is accepted");
    Check(IsPoolSizeAutoLiteral(" Auto \t"), "auto is case-insensitive and trimmed");
    Check(!IsPoolSizeAutoLiteral("2.0"), "numeric literal is not auto");

    {
        const auto automatic = ParsePoolSizeGb("auto");
        const auto manual = ParsePoolSizeGb(" 16.25 ");
        Check(automatic.IsAuto(), "auto parses as automatic mode");
        Check(!manual.IsAuto() && NearlyEqual(manual.requestedManualGb, 16.25f),
              "manual request is retained without policy clamping");
        for (const auto invalid : {"", "garbage", "2 GB", "nan", "inf", "-inf"}) {
            Check(ParsePoolSizeGb(invalid).IsAuto(),
                  std::string("invalid/non-finite input falls back to auto: ") + invalid);
        }
        Check(FormatPoolSizeGb(manual) == "16.2",
              "manual setting formats with the compatible one-decimal INI syntax");
    }

    // The requested manual value survives auto/manual toggles without an INI rewrite.
    {
        auto setting = ParsePoolSizeGb("16.0");
        setting.mode = PoolSizeMode::Auto;
        Check(FormatPoolSizeGb(setting) == "auto", "auto persists as one literal");
        setting.mode = PoolSizeMode::Manual;
        Check(FormatPoolSizeGb(setting) == "16.0",
              "manual request returns after toggling out of auto");
    }

    // MiB/GiB conversions are binary and do not round an observed engine lock.
    {
        Check(EnginePoolMbToBytes(1) == jst::core::kBytesPerMiB,
              "one engine MiB converts exactly to bytes");
        Check(EnginePoolMbToBytes(3073) == 3073ull * jst::core::kBytesPerMiB,
              "non-GiB engine value preserves every MiB");
        Check(!EnginePoolMbToBytes(0) && !EnginePoolMbToBytes(-1),
              "non-positive engine values are not ready");
        Check(PoolSizeGbToBytes(2.0f) == 2ull * jst::core::kBytesPerGiB,
              "two configured GiB convert exactly to bytes");
        Check(NearlyEqual(PoolSizeBytesToGb(1536ull * jst::core::kBytesPerMiB), 1.5f),
              "1536 MiB converts exactly to 1.5 GiB");
    }

    {
        const auto limits = MakePoolSizePolicy(GiB(24)).limits;
        Check(ValidateEnginePoolMb(0, limits) == EnginePoolCandidateValidity::NotReady,
              "zero candidate is not ready");
        Check(ValidateEnginePoolMb(100, limits) == EnginePoolCandidateValidity::BelowMinimum,
              "candidate below 0.5 GiB is rejected");
        Check(ValidateEnginePoolMb(3000, limits) == EnginePoolCandidateValidity::Valid,
              "3000 MiB candidate is valid");
        Check(ValidateEnginePoolMb(20'000, limits) == EnginePoolCandidateValidity::AboveMaximum,
              "candidate above GPU policy is rejected");
    }

    {
        const auto policy = MakePoolSizePolicy(GiB(24));
        const auto status = FormatStreamingPoolStatus(StreamingPoolSnapshot{
            .state = StreamingPoolState::Manual,
            .lockedBytes = PoolSizeGbToBytes(16.8f, policy.limits),
            .effectiveGb = 16.8f,
            .requestedManualGb = 20.0f,
            .policy = policy,
        });
        Check(status.find("GPU 24.0 GB") != std::string::npos,
              "status reports detected VRAM");
        Check(status.find("requested 20.0 GB") != std::string::npos,
              "status reports normalized/manual difference");
    }

    Check(NearlyEqual(NormalizePoolSizeGb(
                          std::numeric_limits<float>::infinity()),
                      kPoolSizeDefaultFallbackGb),
          "non-finite runtime input normalizes to fallback");
}
