#ifndef NEXUSFS_CLUSTER_METADATA_COORDINATOR_HPP
#define NEXUSFS_CLUSTER_METADATA_COORDINATOR_HPP

#include "nexusfs/cluster/metadata_ownership.hpp"
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

class ManifestStore;

}

namespace nexusfs::cluster
{

struct MetadataPublicationReport
{
    MetadataOwner owner;

    bool owner_acknowledged{false};
    bool owner_created{false};

    bool local_cache_created{false};
};

struct ManifestRecoveryReport
{
    bool already_local{false};
    bool recovered{false};

    std::size_t owner_attempts{0};

    std::string source_owner_node_id;

    std::vector<ReplicationFailure>
        failures;
};

/*
 * Coordinates deterministic metadata ownership.
 *
 * Publication is strict: when the selected owner is remote, its
 * durable acknowledgement must arrive before the local manifest
 * cache is published.
 *
 * Recovery tries ordered remote owners, verifies canonical bytes
 * and durably republishes the manifest into the local store.
 */
class MetadataCoordinator final
{
public:
    MetadataCoordinator(
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

    [[nodiscard]] MetadataPublicationReport
    publish_manifest(
        const std::string& manifest_id,
        const std::vector<std::uint8_t>& encoded_manifest,
        storage::ManifestStore& local_manifest_store
    );

    [[nodiscard]] ManifestRecoveryReport
    recover_manifest(
        const std::string& manifest_id,
        storage::ManifestStore& local_manifest_store
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
