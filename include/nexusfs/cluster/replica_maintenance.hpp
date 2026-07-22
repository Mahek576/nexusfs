#ifndef NEXUSFS_CLUSTER_REPLICA_MAINTENANCE_HPP
#define NEXUSFS_CLUSTER_REPLICA_MAINTENANCE_HPP

#include "nexusfs/cluster/peer_transport.hpp"
#include "nexusfs/cluster/replica_repair.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
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

struct ReplicaMaintenanceReport
{
    std::size_t chunks_scanned{0};
    std::size_t local_chunks_recovered{0};

    std::size_t remote_replicas_observed{0};
    std::size_t remote_replicas_created{0};

    std::size_t peer_failures{0};
    std::size_t under_replicated_chunks{0};

    bool fully_repaired{false};
};

/*
 * Proactively validates the configured replication policy.
 *
 * For each unique locally referenced chunk:
 *
 * 1. Recover a missing local copy when possible.
 * 2. Probe deterministic peer candidates.
 * 3. Reuse healthy existing replicas.
 * 4. Create a replacement replica when a selected peer is down.
 * 5. Report any chunk that remains under-replicated.
 */
class ReplicaMaintenanceCoordinator final
{
public:
    ReplicaMaintenanceCoordinator(
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

    [[nodiscard]] ReplicaMaintenanceReport
    repair_chunks(
        const std::vector<std::string>& chunk_hashes,
        storage::ChunkStore& local_chunk_store
    );

private:
    std::shared_ptr<
        ClusterNodeFoundation
    > cluster_node_;

    std::size_t replication_factor_;

    PeerTransport transport_;

    ReplicaRepairCoordinator
        recovery_coordinator_;

    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry_;

    std::shared_ptr<
        observability::JsonLogger
    > logger_;
};

}

#endif
