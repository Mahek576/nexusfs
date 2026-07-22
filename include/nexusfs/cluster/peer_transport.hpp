#ifndef NEXUSFS_CLUSTER_PEER_TRANSPORT_HPP
#define NEXUSFS_CLUSTER_PEER_TRANSPORT_HPP

#include "nexusfs/cluster/cluster_node_foundation.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nexusfs::observability
{

class JsonLogger;
class MetricsRegistry;

}

namespace nexusfs::cluster
{

enum class RemoteChunkStoreResult
{
    stored,
    already_exists
};

struct ReplicationFailure
{
    std::string peer_node_id;
    std::string error;
};

struct ReplicationReport
{
    std::size_t requested_remote_replicas{0};
    std::size_t selected_peers{0};
    std::size_t acknowledged_replicas{0};

    std::vector<std::string>
        acknowledged_peer_ids;

    std::vector<ReplicationFailure>
        failures;

    bool satisfied{false};
};

/*
 * Synchronous peer-to-peer HTTP transport.
 *
 * Every request carries the local cluster and node identity.
 * Network failures are reflected in the peer-health registry.
 */
class PeerTransport final
{
public:
    explicit PeerTransport(
        std::shared_ptr<
            ClusterNodeFoundation
        > cluster_node,
        std::chrono::milliseconds timeout =
            std::chrono::milliseconds{
                3000
            }
    );

    void send_heartbeat(
        const PeerDefinition& peer
    );

    [[nodiscard]] bool chunk_exists(
        const PeerDefinition& peer,
        const std::string& chunk_hash
    );

    [[nodiscard]] RemoteChunkStoreResult
    store_chunk(
        const PeerDefinition& peer,
        const std::string& chunk_hash,
        const std::vector<std::uint8_t>& data
    );

    [[nodiscard]] std::vector<std::uint8_t>
    load_chunk(
        const PeerDefinition& peer,
        const std::string& chunk_hash
    );

private:
    void record_peer_success(
        const PeerDefinition& peer
    ) noexcept;

    void record_peer_failure(
        const PeerDefinition& peer,
        const std::string& error
    ) noexcept;

    std::shared_ptr<
        ClusterNodeFoundation
    > cluster_node_;

    std::chrono::milliseconds timeout_;
};

/*
 * Deterministic rendezvous-hash replica placement plus transport
 * acknowledgement tracking.
 *
 * total_replication_factor includes the local copy. For example,
 * factor 2 requests one remote replica.
 */
class ReplicationCoordinator final
{
public:
    explicit ReplicationCoordinator(
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

    [[nodiscard]] ReplicationReport
    replicate_chunk(
        const std::string& chunk_hash,
        const std::vector<std::uint8_t>& data,
        std::size_t total_replication_factor
    );

    [[nodiscard]] static std::vector<
        PeerDefinition
    > select_replica_peers(
        const std::string& chunk_hash,
        const std::vector<PeerDefinition>& peers,
        std::size_t desired_peer_count
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
