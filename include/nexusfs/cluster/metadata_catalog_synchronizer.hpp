#ifndef NEXUSFS_CLUSTER_METADATA_CATALOG_SYNCHRONIZER_HPP
#define NEXUSFS_CLUSTER_METADATA_CATALOG_SYNCHRONIZER_HPP

#include "nexusfs/cluster/metadata_catalog.hpp"
#include "nexusfs/cluster/metadata_coordinator.hpp"
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

struct MetadataCatalogConflict
{
    std::string manifest_id;

    std::string existing_node_id;
    std::string conflicting_node_id;

    MetadataCatalogEntry existing_entry;
    MetadataCatalogEntry conflicting_entry;
};

struct MetadataCatalogMergeResult
{
    std::vector<MetadataCatalogEntry>
        entries;

    std::vector<MetadataCatalogConflict>
        conflicts;
};

struct MetadataCatalogSyncReport
{
    std::size_t peers_contacted{0};
    std::size_t peers_succeeded{0};
    std::size_t peers_failed{0};

    std::size_t remote_entries_observed{0};
    std::size_t unique_entries_discovered{0};

    std::size_t manifests_already_local{0};
    std::size_t manifests_recovered{0};
    std::size_t manifests_unrecovered{0};

    std::size_t conflicts_detected{0};

    std::vector<ReplicationFailure>
        peer_failures;

    std::vector<MetadataCatalogConflict>
        conflicts;

    std::vector<MetadataCatalogEntry>
        synchronized_entries;

    bool converged{false};
};

/*
 * Additive cluster metadata synchronization.
 *
 * Synchronization never deletes local metadata. Remote catalogs are
 * authenticated and digest-validated by PeerTransport. Catalogs are
 * merged deterministically by manifest ID, conflicts are rejected,
 * and missing manifests are recovered through ordered metadata
 * ownership.
 */
class MetadataCatalogSynchronizer final
{
public:
    MetadataCatalogSynchronizer(
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

    [[nodiscard]] MetadataCatalogSyncReport
    synchronize(
        storage::ManifestStore& local_manifest_store
    );

    [[nodiscard]] static MetadataCatalogMergeResult
    merge_snapshots(
        std::vector<MetadataCatalogSnapshot> snapshots
    );

private:
    std::shared_ptr<
        ClusterNodeFoundation
    > cluster_node_;

    PeerTransport transport_;

    MetadataCoordinator
        metadata_coordinator_;

    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry_;

    std::shared_ptr<
        observability::JsonLogger
    > logger_;
};

}

#endif
