#pragma once

#if !defined(JST_UNIT_TESTS)
#error "graphics_adapter_service_test_access.hpp is test-only"
#endif

#include "core/graphics_adapter_service.hpp"

#include <functional>
#include <optional>

namespace jst::core {

class GraphicsAdapterServiceTestAccess final {
public:
    using Probe = std::function<std::optional<GraphicsAdapterSnapshot>(GraphicsAdapterId)>;

    static void Reset(GraphicsAdapterService& service) {
        service.Stop();
        {
            std::lock_guard lock(service.m_mutex);
            service.m_probeFunction = &GraphicsAdapterService::ProbeAdapter;
            service.m_completedProbeId.reset();
            service.m_pendingProbe.reset();
            service.m_inFlightProbe.reset();
        }
        service.ApplyCandidate({}, true);
    }

    static void SetProbe(GraphicsAdapterService& service, Probe probe) {
        service.Stop();
        service.ApplyCandidate({}, true);
        std::lock_guard lock(service.m_mutex);
        service.m_probeFunction = [probe = std::move(probe)](GraphicsAdapterId id) {
            const auto result = probe ? probe(id) : std::nullopt;
            if (!result) {
                return GraphicsAdapterService::ProbeResult{
                    .snapshot = GraphicsAdapterSnapshot{.id = id},
                    .disposition = GraphicsAdapterService::ProbeDisposition::Retry,
                };
            }
            return GraphicsAdapterService::ProbeResult{
                .snapshot = *result,
                .disposition = GraphicsAdapterService::ProbeDisposition::Complete,
            };
        };
    }

    static void Publish(GraphicsAdapterService& service, GraphicsAdapterSnapshot snapshot) {
        service.ApplyCandidate(std::move(snapshot), true);
    }

    static void ApplyCandidate(
        GraphicsAdapterService& service, GraphicsAdapterSnapshot snapshot) {
        service.ApplyCandidate(std::move(snapshot));
    }

    [[nodiscard]] static GraphicsAdapterSnapshot Classify(
        GraphicsAdapterId id, uint64_t dedicatedBytes, bool software) noexcept {
        return GraphicsAdapterService::ClassifyAdapter(id, dedicatedBytes, software);
    }
};

} // namespace jst::core
