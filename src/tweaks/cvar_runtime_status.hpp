#pragma once

#include "core/cvar_system.hpp"
#include "tweak.hpp"

#include <span>

namespace jst::tweaks {

[[nodiscard]] RuntimeEditResult CVarEditResult(
    const jst::core::CVarQueueResult& result);

[[nodiscard]] TweakRuntimeStatus CVarTicketStatus(
    const jst::core::CVarCommandTicket& ticket,
    TweakRuntimeState settledState = TweakRuntimeState::Applied);

[[nodiscard]] TweakRuntimeStatus CVarTicketsStatus(
    std::span<const jst::core::CVarCommandTicket> tickets,
    TweakRuntimeState emptyState = TweakRuntimeState::Applied);

} // namespace jst::tweaks
