#include "cvar_startup_reconciler.hpp"

#include <utility>

namespace jst::core {

void CVarStartupReconciler::Track(
    std::wstring_view name,
    std::wstring value,
    CVarValueKind kind,
    uint64_t generation) {
    if (m_started) {
        // The first post-Tick pass freezes the startup set. A later explicit
        // runtime command supersedes startup protection for this CVar.
        Cancel(name);
        return;
    }
    m_targets.insert_or_assign(
        std::wstring(name),
        Target{
            .value = std::move(value),
            .kind = kind,
            .generation = generation,
        });
}

size_t CVarStartupReconciler::Start(
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds duration,
    std::chrono::milliseconds interval) {
    m_started = true;
    m_corrections = 0;
    if (m_targets.empty()) {
        m_deadline.reset();
        m_nextEvaluation = {};
        m_interval = {};
        return 0;
    }
    m_deadline = now + duration;
    m_interval = interval;
    m_nextEvaluation = now + interval;
    return m_targets.size();
}

void CVarStartupReconciler::CaptureApplied(
    std::wstring_view name,
    uint64_t generation,
    std::optional<int32_t> valueWord) {
    const auto found = m_targets.find(name);
    if (found == m_targets.end() || found->second.generation != generation) {
        return;
    }
    if (valueWord) {
        found->second.expectedValueWord = *valueWord;
    } else {
        m_targets.erase(found);
    }
}

void CVarStartupReconciler::Cancel(std::wstring_view name) {
    const auto found = m_targets.find(name);
    if (found == m_targets.end()) {
        return;
    }
    auto expected = CVarCorrectionPermitState::Eligible;
    if (!found->second.permit->compare_exchange_strong(
            expected,
            CVarCorrectionPermitState::CancelledBeforeInvoke,
            std::memory_order_acq_rel)) {
        expected = CVarCorrectionPermitState::Invoking;
        (void)found->second.permit->compare_exchange_strong(
            expected,
            CVarCorrectionPermitState::SupersededDuringInvoke,
            std::memory_order_acq_rel);
    }
    m_targets.erase(found);
}

bool CVarStartupReconciler::Cancel(
    std::wstring_view name,
    uint64_t generation) {
    const auto found = m_targets.find(name);
    if (found == m_targets.end() || found->second.generation != generation) {
        return false;
    }
    auto expected = CVarCorrectionPermitState::Eligible;
    if (!found->second.permit->compare_exchange_strong(
            expected,
            CVarCorrectionPermitState::CancelledBeforeInvoke,
            std::memory_order_acq_rel)) {
        expected = CVarCorrectionPermitState::Invoking;
        (void)found->second.permit->compare_exchange_strong(
            expected,
            CVarCorrectionPermitState::SupersededDuringInvoke,
            std::memory_order_acq_rel);
    }
    m_targets.erase(found);
    return true;
}

void CVarStartupReconciler::Clear() noexcept {
    m_targets.clear();
    m_deadline.reset();
    m_nextEvaluation = {};
    m_interval = {};
    m_corrections = 0;
    m_started = false;
}

CVarStartupEvaluation CVarStartupReconciler::Evaluate(
    std::chrono::steady_clock::time_point now) {
    CVarStartupEvaluation evaluation;
    if (!m_deadline) {
        return evaluation;
    }
    if (now >= *m_deadline) {
        evaluation.finishedCorrectionCount = m_corrections;
        m_targets.clear();
        m_deadline.reset();
        m_nextEvaluation = {};
        return evaluation;
    }
    if (now < m_nextEvaluation) {
        return evaluation;
    }

    m_nextEvaluation = now + m_interval;
    evaluation.targets.reserve(m_targets.size());
    for (const auto& [name, target] : m_targets) {
        if (!target.expectedValueWord) {
            continue;
        }
        evaluation.targets.push_back(CVarStartupTarget{
            .name = name,
            .value = target.value,
            .kind = target.kind,
            .generation = target.generation,
            .expectedValueWord = *target.expectedValueWord,
            .permit = target.permit,
        });
    }
    return evaluation;
}

bool CVarStartupReconciler::IsCurrent(
    std::wstring_view name,
    uint64_t generation) const {
    const auto found = m_targets.find(name);
    return found != m_targets.end() &&
        found->second.generation == generation;
}

bool CVarStartupReconciler::TryBeginCorrection(
    std::wstring_view name,
    uint64_t generation,
    const std::shared_ptr<std::atomic<CVarCorrectionPermitState>>& permit) {
    const auto found = m_targets.find(name);
    if (found == m_targets.end() || found->second.generation != generation ||
        found->second.permit != permit) {
        return false;
    }
    auto expected = CVarCorrectionPermitState::Eligible;
    return permit->compare_exchange_strong(
        expected,
        CVarCorrectionPermitState::Invoking,
        std::memory_order_acq_rel);
}

bool CVarStartupReconciler::RecordCorrection(
    std::wstring_view name,
    uint64_t generation,
    const std::shared_ptr<std::atomic<CVarCorrectionPermitState>>& permit,
    std::optional<int32_t> valueWord) {
    const auto found = m_targets.find(name);
    if (found == m_targets.end() || found->second.generation != generation ||
        found->second.permit != permit) {
        return false;
    }
    auto expected = CVarCorrectionPermitState::Invoking;
    if (!permit->compare_exchange_strong(
            expected,
            CVarCorrectionPermitState::Complete,
            std::memory_order_acq_rel)) {
        return false;
    }
    if (!valueWord) {
        m_targets.erase(found);
        return false;
    }
    found->second.expectedValueWord = *valueWord;
    permit->store(CVarCorrectionPermitState::Eligible, std::memory_order_release);
    ++m_corrections;
    return true;
}

} // namespace jst::core
