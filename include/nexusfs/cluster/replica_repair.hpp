#ifndef NEXUSFS_CLUSTER_REPLICA_REPAIR_HPP
#define NEXUSFS_CLUSTER_REPLICA_REPAIR_HPP

#include "nexusfs/cluster/peer_transport.hpp"

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

struct ChunkRecoveryReport
{
    bool already_local{false};
    bool recovered{false};

    std::size_t peer_attempts{0};

    std::string source_peer_node_id;

    std::vector<ReplicationFailure>
        failures;
};

/*
 * Recovers a missing local content-addressed chunk from configured
 * peers.
 *
 * Remote bytes are verified by PeerTransport against the requested
 * SHA-256 hash before they are durably published into the local
 * ChunkStore. The local copy is loaded again after publication.
 */
class ReplicaRepairCoordinator final
{
public:
    explicit ReplicaRepairCoordinator(
        std::shared_ptr<
            ClusterNodeFoundation
        > cluster_node,
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

    [[nodiscard]] ChunkRecoveryReport
    recover_chunk(
        const std::string& chunk_hash,
        storage::ChunkStore& local_chunk_store
    );

private:
    std::shared_ptr<
        ClusterNodeFoundation
    > cluster_node_;

    PeerTransport transport_;

    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry_;

    std::shared_ptr<
        observability::JsonLogger
    > logger_;
};

}

#endif
