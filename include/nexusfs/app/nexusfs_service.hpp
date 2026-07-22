#ifndef NEXUSFS_APP_NEXUSFS_SERVICE_HPP
#define NEXUSFS_APP_NEXUSFS_SERVICE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nexusfs::cluster
{

class ClusterNodeFoundation;
class MetadataCatalogSynchronizer;
class MetadataCoordinator;
class PlacementRebalancer;
class ReplicaMaintenanceCoordinator;
class ReplicaRepairCoordinator;
class ReplicationCoordinator;

}

namespace nexusfs::observability
{

class JsonLogger;
class MetricsRegistry;

}

namespace nexusfs::app
{

struct StoreFileResult
{
    std::filesystem::path source_path;
    std::string manifest_id;
    std::string original_filename;
    std::uint64_t file_size;
    std::size_t chunk_count;
    std::size_t chunks_stored;
    std::size_t chunks_reused;
    std::uint64_t bytes_processed;
    std::size_t encoded_manifest_size;
    bool manifest_stored;

    std::size_t replication_factor;
    std::size_t remote_replica_acknowledgements;
    bool replication_satisfied;

    std::string metadata_owner_node_id;
    bool metadata_owner_local;
    bool metadata_owner_acknowledged;
};

struct RestoreFileResult
{
    std::string manifest_id;
    std::string original_filename;
    std::uint64_t file_size;
    std::size_t chunk_count;
    std::filesystem::path output_path;
    std::uint64_t bytes_written;
    std::size_t chunks_loaded;
};

struct InspectedChunk
{
    std::size_t index;
    std::string hash;
    bool present;
};

struct InspectFileResult
{
    std::string manifest_id;
    std::string original_filename;
    std::uint64_t file_size;
    std::size_t configured_chunk_size;
    std::size_t encoded_manifest_size;
    std::vector<InspectedChunk> chunks;
    std::size_t available_chunks;
    std::size_t missing_chunks;
};

struct VerifiedChunkResult
{
    std::size_t index;
    std::string hash;
    std::uint64_t bytes_verified;
};

struct VerifyFileResult
{
    std::string manifest_id;
    std::string original_filename;
    std::uint64_t file_size;
    std::size_t chunk_count;
    std::vector<VerifiedChunkResult> verified_chunks;
    std::uint64_t total_bytes_verified;
};

struct StoredFileSummary
{
    std::string manifest_id;
    std::string original_filename;
    std::uint64_t file_size;
    std::size_t configured_chunk_size;
    std::size_t chunk_count;
    std::size_t missing_chunks;
};

struct ListFilesResult
{
    std::vector<StoredFileSummary> files;
    std::size_t complete_manifests;
    std::size_t incomplete_manifests;
};

struct SynchronizeMetadataCatalogResult
{
    std::vector<StoredFileSummary> files;

    std::size_t peers_contacted;
    std::size_t peers_succeeded;
    std::size_t peers_failed;

    std::size_t remote_entries_observed;
    std::size_t unique_entries_discovered;

    std::size_t manifests_already_local;
    std::size_t manifests_recovered;
    std::size_t manifests_unrecovered;

    std::size_t conflicts_detected;

    bool converged;
};

struct RebalanceClusterResult
{
    std::string status;
    std::string operation_id;
    std::string request_digest;

    std::uint64_t expected_membership_epoch;
    std::uint64_t observed_membership_epoch;
    std::uint64_t replication_factor;

    std::uint64_t manifests_scanned;
    std::uint64_t unique_chunks_scanned;
    std::uint64_t targets_planned;

    std::uint64_t replicas_observed;
    std::uint64_t replicas_created;

    std::uint64_t peer_failures;
    std::uint64_t under_replicated_chunks;

    bool converged;
    bool replayed;
    bool applied;
};

struct RepairReplicasResult
{
    std::size_t manifests_scanned;
    std::size_t unique_chunks_scanned;

    std::size_t local_chunks_recovered;

    std::size_t remote_replicas_observed;
    std::size_t remote_replicas_created;

    std::size_t peer_failures;
    std::size_t under_replicated_chunks;

    bool fully_repaired;
};

/*
 * Internal process-wide synchronization state.
 *
 * Services constructed for equivalent storage-root paths share the
 * same state, preventing independently created service objects from
 * racing against each other inside one NexusFS process.
 */
class NexusFsServiceConcurrencyState;

class NexusFsService
{
public:
    explicit NexusFsService(
        std::filesystem::path storage_root,
        std::size_t default_chunk_size = 1024
    );

    NexusFsService(
        std::filesystem::path storage_root,
        std::size_t default_chunk_size,
        std::shared_ptr<
            cluster::ClusterNodeFoundation
        > cluster_node,
        std::size_t replication_factor,
        bool strict_replication,
        std::shared_ptr<
            observability::MetricsRegistry
        > metrics_registry = nullptr,
        std::shared_ptr<
            observability::JsonLogger
        > logger = nullptr
    );

    /*
     * Storage operation locking model:
     *
     * store_file():
     *     Exclusive storage lock because it publishes chunks and
     *     a manifest into the shared content-addressed store.
     *
     * restore_file():
     *     Shared storage lock because it only reads NexusFS storage.
     *     Restoration output paths are protected separately.
     *
     * inspect_file(), verify_file(), list_files():
     *     Shared storage locks, allowing concurrent readers.
     */
    [[nodiscard]] StoreFileResult store_file(
        const std::filesystem::path& source_path
    ) const;

    [[nodiscard]] RestoreFileResult restore_file(
        const std::string& manifest_id,
        const std::filesystem::path& output_path
    ) const;

    [[nodiscard]] InspectFileResult inspect_file(
        const std::string& manifest_id
    ) const;

    [[nodiscard]] VerifyFileResult verify_file(
        const std::string& manifest_id
    ) const;

    [[nodiscard]] ListFilesResult list_files() const;

    [[nodiscard]] SynchronizeMetadataCatalogResult
    synchronize_metadata_catalog() const;

    [[nodiscard]] RebalanceClusterResult
    rebalance_cluster(
        std::string operation_id,
        std::uint64_t expected_membership_epoch
    ) const;

    [[nodiscard]] RepairReplicasResult
    repair_replicas() const;

    [[nodiscard]] const std::filesystem::path&
    storage_root() const noexcept;

    [[nodiscard]] std::size_t
    default_chunk_size() const noexcept;

private:
    std::filesystem::path storage_root_;
    std::size_t default_chunk_size_;

    std::size_t replication_factor_{
        1
    };

    bool strict_replication_{
        true
    };

    std::shared_ptr<
        cluster::PlacementRebalancer
    > placement_rebalancer_;

    std::shared_ptr<
        cluster::MetadataCatalogSynchronizer
    > metadata_catalog_synchronizer_;

    std::shared_ptr<
        cluster::MetadataCoordinator
    > metadata_coordinator_;

    std::shared_ptr<
        cluster::ReplicationCoordinator
    > replication_coordinator_;

    std::shared_ptr<
        cluster::ReplicaRepairCoordinator
    > replica_repair_coordinator_;

    std::shared_ptr<
        cluster::ReplicaMaintenanceCoordinator
    > replica_maintenance_coordinator_;

    void repair_missing_manifest(
        const std::string& manifest_id
    ) const;

    void repair_missing_manifest_chunks(
        const std::string& manifest_id
    ) const;

    /*
     * shared_ptr deliberately preserves the original copyability of
     * NexusFsService. Copies of one service continue sharing the same
     * synchronization state.
     */
    std::shared_ptr<
        NexusFsServiceConcurrencyState
    > concurrency_state_;
};

}

#endif