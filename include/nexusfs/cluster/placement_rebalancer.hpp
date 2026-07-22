#ifndef NEXUSFS_CLUSTER_PLACEMENT_REBALANCER_HPP
#define NEXUSFS_CLUSTER_PLACEMENT_REBALANCER_HPP

#include "nexusfs/cluster/operation_journal.hpp"
#include "nexusfs/cluster/peer_transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nexusfs::observability
{

class JsonLogger;
class MetricsRegistry;

}

namespace nexusfs::storage
{

class ChunkStore;

}

namespace nexusfs::cluster
{

enum class RebalanceStatus
{
    completed,
    replayed,
    stale_membership_epoch
};

[[nodiscard]] std::string_view
rebalance_status_name(
    RebalanceStatus status
) noexcept;

struct PlacementRebalanceReport
{
    RebalanceStatus status{
        RebalanceStatus::completed
    };

    std::string operation_id;
    std::string request_digest;

    std::uint64_t expected_membership_epoch{0};
    std::uint64_t observed_membership_epoch{0};
    std::uint64_t replication_factor{0};

    std::uint64_t chunks_scanned{0};
    std::uint64_t targets_planned{0};

    std::uint64_t replicas_observed{0};
    std::uint64_t replicas_created{0};

    std::uint64_t peer_failures{0};
    std::uint64_t under_replicated_chunks{0};

    bool converged{false};
    bool replayed{false};
    bool applied{false};
};

/*
 * Recalculates deterministic remote placement for the current
 * membership epoch.
 *
 * The operation is additive: it creates missing target replicas but
 * never deletes an existing replica. This keeps stale or partial
 * cluster views from causing destructive movement.
 */
class PlacementRebalancer final
{
public:
    PlacementRebalancer(
        std::shared_ptr<
            ClusterNodeFoundation
        > cluster_node,
        std::size_t replication_factor,
        std::chrono::milliseconds timeout =
            std::chrono::milliseconds{
                3000
            },
        std::shared_ptr<
            observability::MetricsRegistry
        > metrics_registry = nullptr,
        std::shared_ptr<
            observability::JsonLogger
        > logger = nullptr
    );

    [[nodiscard]] PlacementRebalanceReport
    rebalance(
        const std::vector<std::string>& chunk_hashes,
        storage::ChunkStore& local_chunk_store,
        std::string operation_id,
        std::uint64_t expected_membership_epoch
    );

private:
    std::shared_ptr<
        ClusterNodeFoundation
    > cluster_node_;

    std::size_t replication_factor_;

    PeerTransport transport_;
    OperationJournal journal_;

    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry_;

    std::shared_ptr<
        observability::JsonLogger
    > logger_;
};

}

#endif
