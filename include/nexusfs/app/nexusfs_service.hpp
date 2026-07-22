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
        cluster::ReplicationCoordinator
    > replication_coordinator_;

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