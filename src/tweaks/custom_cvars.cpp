#include "custom_cvars.hpp"

#include "core/config.hpp"
#include "core/logging.hpp"
#include "core/string_utils.hpp"
#include "cvar_runtime_status.hpp"
#include "custom_cvar_value.hpp"

#include <format>

namespace jst::tweaks {

std::expected<TweakActivation, std::string> CustomCVarsTweak::Configure(
    const jst::core::Config& config) {
    m_entries.clear();
    m_initialManagedConflicts = 0;
    const auto* section = config.GetSection(Name());
    if (section) {
        m_entries.reserve(section->size());
        for (const auto& [name, rawValue] : *section) {
            if (name == "Enabled") {
                continue;
            }
            if (name.size() < 2) {
                JST_LOG_WARNING("Skipping invalid CVar name '{}'.", name);
                continue;
            }
            const auto normalized = NormalizeCustomCVarValue(rawValue);
            if (!normalized) {
                JST_LOG_ERROR(
                    "Skipping CVar '{}' with invalid numeric value '{}'.",
                    name,
                    rawValue);
                continue;
            }
            m_entries.push_back(Entry{
                .name = jst::core::utils::Utf8ToWide(name),
                .value = jst::core::utils::Utf8ToWide(*normalized),
            });
        }
    }
    return ITweak::Configure(config);
}

std::expected<void, std::string> CustomCVarsTweak::Prepare(
    [[maybe_unused]] jst::core::HookEngine& hooks) {
    auto& cvars = jst::core::CVarSystem::Instance();
    size_t queued = 0;
    size_t replaced = 0;
    size_t rejected = 0;
    m_initialManagedConflicts = 0;
    m_tickets.clear();
    m_tickets.reserve(m_entries.size());

    for (const auto& entry : m_entries) {
        const auto result = cvars.SetString(
            entry.name,
            entry.value,
            jst::core::CVarCommandSource::Custom);
        if (result.rejection ==
            jst::core::CVarRejectionReason::ManagedConflict) {
            ++m_initialManagedConflicts;
            continue;
        }
        switch (result.result) {
        case jst::core::CVarSetResult::Queued:
            ++queued;
            break;
        case jst::core::CVarSetResult::Replaced:
            ++replaced;
            break;
        case jst::core::CVarSetResult::Rejected:
            ++rejected;
            break;
        }
        if (result.Accepted()) {
            m_tickets.push_back(result.ticket);
        }
    }

    JST_LOG_INFO(
        "CustomCVars: {} queued, {} coalesced, {} conflicts, {} rejected.",
        queued,
        replaced,
        m_initialManagedConflicts,
        rejected);
    if (rejected != 0 && queued == 0 && replaced == 0 && !m_entries.empty()) {
        return std::unexpected("all custom CVar requests were rejected");
    }
    return {};
}

void CustomCVarsTweak::Shutdown() {
    m_tickets.clear();
    m_entries.clear();
    m_initialManagedConflicts = 0;
}

std::optional<TweakRuntimeStatus> CustomCVarsTweak::RuntimeStatus() const {
    auto status = CVarTicketsStatus(m_tickets);
    if (status.state == TweakRuntimeState::Failed) {
        return status;
    }

    size_t managedConflicts = m_initialManagedConflicts;
    for (const auto& ticket : m_tickets) {
        const auto snapshot = ticket.Snapshot();
        if (snapshot.state == jst::core::CVarCommandState::Superseded &&
            snapshot.supersedeReason ==
                jst::core::CVarSupersedeReason::ManagedPriority) {
            ++managedConflicts;
        }
    }
    if (managedConflicts != 0) {
        status.message = std::format(
            "{} custom CVar{} skipped because specialized tweaks control {}.",
            managedConflicts,
            managedConflicts == 1 ? " was" : "s were",
            managedConflicts == 1 ? "it" : "them");
    }
    return status;
}

} // namespace jst::tweaks
