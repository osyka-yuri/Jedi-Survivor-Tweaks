#pragma once

namespace jst::core {

/// Installs the ASI-only early observer for the main executable's normal and
/// delay-load D3D12CreateDevice imports. It performs only PE/IAT work and is
/// safe to call before the asynchronous application bootstrap begins.
[[nodiscard]] bool InstallD3D12DeviceObserverEarly() noexcept;

} // namespace jst::core
