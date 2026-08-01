#include "cvar_runtime_status.hpp"

namespace jst::tweaks {

RuntimeEditResult CVarEditResult(
    const jst::core::CVarQueueResult& result) {
    const auto snapshot = result.ticket.Snapshot();
    if (!result.Accepted() ||
        snapshot.state == jst::core::CVarCommandState::Failed) {
        return RejectedEdit(snapshot.diagnostic);
    }
    return snapshot.state == jst::core::CVarCommandState::Applied
        ? AppliedEdit()
        : QueuedEdit();
}

TweakRuntimeStatus CVarTicketStatus(
    const jst::core::CVarCommandTicket& ticket,
    TweakRuntimeState settledState) {
    if (!ticket) {
        return TweakRuntimeStatus{.state = settledState};
    }

    const auto snapshot = ticket.Snapshot();
    switch (snapshot.state) {
    case jst::core::CVarCommandState::Pending:
        return TweakRuntimeStatus{.state = TweakRuntimeState::Pending};
    case jst::core::CVarCommandState::Failed:
        return TweakRuntimeStatus{
            .state = TweakRuntimeState::Failed,
            .message = snapshot.diagnostic,
        };
    case jst::core::CVarCommandState::Applied:
    case jst::core::CVarCommandState::Superseded:
        return TweakRuntimeStatus{.state = settledState};
    }
    return TweakRuntimeStatus{.state = settledState};
}

TweakRuntimeStatus CVarTicketsStatus(
    std::span<const jst::core::CVarCommandTicket> tickets,
    TweakRuntimeState emptyState) {
    bool hasTicket = false;
    bool pending = false;
    for (const auto& ticket : tickets) {
        if (!ticket) {
            continue;
        }
        hasTicket = true;
        const auto snapshot = ticket.Snapshot();
        if (snapshot.state == jst::core::CVarCommandState::Failed) {
            return TweakRuntimeStatus{
                .state = TweakRuntimeState::Failed,
                .message = snapshot.diagnostic,
            };
        }
        pending = pending ||
            snapshot.state == jst::core::CVarCommandState::Pending;
    }
    if (pending) {
        return TweakRuntimeStatus{.state = TweakRuntimeState::Pending};
    }
    return TweakRuntimeStatus{
        .state = hasTicket ? TweakRuntimeState::Applied : emptyState,
    };
}

} // namespace jst::tweaks
