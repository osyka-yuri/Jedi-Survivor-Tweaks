#pragma once

#include "cvar_name.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jst::core {

enum class CVarValueKind : uint8_t {
    Opaque,
    Integer,
    Float,
};

// The reconciler owns the correction permit, not the engine call.  CVarSystem
// changes this state under its cache mutex immediately before/after an
// unlocked setter invocation; a live command can therefore cancel an eligible
// correction or supersede one that is already inside the setter.
enum class CVarCorrectionPermitState : uint8_t {
    Eligible,
    Invoking,
    CancelledBeforeInvoke,
    SupersededDuringInvoke,
    Complete,
};

struct CVarStartupTarget {
    std::wstring name;
    std::wstring value;
    CVarValueKind kind = CVarValueKind::Opaque;
    uint64_t generation = 0;
    int32_t expectedValueWord = 0;
    std::shared_ptr<std::atomic<CVarCorrectionPermitState>> permit;
};

struct CVarStartupEvaluation {
    std::vector<CVarStartupTarget> targets;
    std::optional<size_t> finishedCorrectionCount;
};

/**
 * Tracks values written during startup and schedules bounded value-only
 * reconciliation. The owner serializes every call with its cache mutex;
 * this class performs no engine access and owns no synchronization.
 */
class CVarStartupReconciler final {
public:
    void Track(
        std::wstring_view name,
        std::wstring value,
        CVarValueKind kind,
        uint64_t generation);
    [[nodiscard]] size_t Start(
        std::chrono::steady_clock::time_point now,
        std::chrono::milliseconds duration,
        std::chrono::milliseconds interval);
    void CaptureApplied(
        std::wstring_view name,
        uint64_t generation,
        std::optional<int32_t> valueWord);
    void Cancel(std::wstring_view name);
    [[nodiscard]] bool Cancel(
        std::wstring_view name,
        uint64_t generation);
    void Clear() noexcept;

    [[nodiscard]] CVarStartupEvaluation Evaluate(
        std::chrono::steady_clock::time_point now);
    [[nodiscard]] bool IsCurrent(
        std::wstring_view name,
        uint64_t generation) const;
    [[nodiscard]] bool TryBeginCorrection(
        std::wstring_view name,
        uint64_t generation,
        const std::shared_ptr<std::atomic<CVarCorrectionPermitState>>& permit);
    [[nodiscard]] bool RecordCorrection(
        std::wstring_view name,
        uint64_t generation,
        const std::shared_ptr<std::atomic<CVarCorrectionPermitState>>& permit,
        std::optional<int32_t> valueWord);

private:
    struct Target {
        std::wstring value;
        CVarValueKind kind = CVarValueKind::Opaque;
        uint64_t generation = 0;
        std::optional<int32_t> expectedValueWord;
        std::shared_ptr<std::atomic<CVarCorrectionPermitState>> permit =
            std::make_shared<std::atomic<CVarCorrectionPermitState>>(
                CVarCorrectionPermitState::Eligible);
    };

    using Targets = std::unordered_map<
        std::wstring,
        Target,
        CVarNameHash,
        CVarNameEqual>;

    Targets m_targets;
    std::optional<std::chrono::steady_clock::time_point> m_deadline;
    std::chrono::steady_clock::time_point m_nextEvaluation{};
    std::chrono::milliseconds m_interval{};
    size_t m_corrections = 0;
    bool m_started = false;
};

} // namespace jst::core
