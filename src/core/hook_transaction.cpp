#include "hook_transaction.hpp"

namespace jst::core::detail {

std::vector<HookError>
RunHookTransaction(std::span<const HookTransactionStep> steps) {
    size_t installedCount = 0;
    for (; installedCount < steps.size(); ++installedCount) {
        Hook* h = steps[installedCount].hook;
        auto installed = h->Install();
        if (installed) {
            continue;
        }

        std::vector<HookError> errors;
        HookError failure = std::move(installed).error();
        failure.site = std::string(steps[installedCount].site);
        errors.push_back(std::move(failure));
        auto failedStepRollback = h->Uninstall();
        if (!failedStepRollback) {
            errors.push_back(std::move(failedStepRollback).error());
        }
        while (installedCount > 0) {
            --installedCount;
            h = steps[installedCount].hook;
            auto rolledBack = h->Uninstall();
            if (!rolledBack) {
                errors.push_back(std::move(rolledBack).error());
            }
        }
        return errors;
    }
    return {};
}

} // namespace jst::core::detail
