#include "nexusfs/app/nexusfs_service.hpp"

#include "nexusfs/cluster/peer_transport.hpp"
#include "nexusfs/cluster/metadata_coordinator.hpp"
#include "nexusfs/cluster/metadata_catalog_synchronizer.hpp"
#include "nexusfs/cluster/replica_repair.hpp"
#include "nexusfs/cluster/replica_maintenance.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/chunker.hpp"
#include "nexusfs/storage/file_manifest.hpp"
#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/file_reconstructor.hpp"
#include "nexusfs/storage/file_verifier.hpp"
#include "nexusfs/storage/manifest_store.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nexusfs::app
{

class NexusFsServiceConcurrencyState
{
public:
    std::shared_mutex storage_mutex;

    [[nodiscard]] std::shared_ptr<std::mutex>
    output_mutex_for(
        const std::filesystem::path& output_path
    )
    {
        const std::string key =
            normalized_path_key(
                output_path
            );

        const std::lock_guard lock{
            output_mutex_registry_mutex_
        };

        remove_expired_output_mutexes();

        const auto existing =
            output_mutexes_.find(
                key
            );

        if (
            existing !=
            output_mutexes_.end()
        )
        {
            if (
                const std::shared_ptr<std::mutex>
                    mutex =
                        existing->second.lock()
            )
            {
                return mutex;
            }
        }

        const auto mutex =
            std::make_shared<std::mutex>();

        output_mutexes_[key] =
            mutex;

        return mutex;
    }

private:
    static std::filesystem::path
    normalized_absolute_path(
        const std::filesystem::path& path
    )
    {
        std::error_code error;

        std::filesystem::path normalized =
            std::filesystem::absolute(
                path,
                error
            );

        if (error)
        {
            error.clear();

            normalized =
                path;
        }

        normalized =
            normalized.lexically_normal();

        const std::filesystem::path
            canonical_path =
                std::filesystem::weakly_canonical(
                    normalized,
                    error
                );

        if (!error)
        {
            normalized =
                canonical_path;
        }

        return normalized;
    }

    static std::string normalized_path_key(
        const std::filesystem::path& path
    )
    {
        std::string key =
            normalized_absolute_path(
                path
            ).generic_string();

#ifdef _WIN32
        /*
         * Windows paths are normally case-insensitive.
         * Normalizing ASCII case ensures equivalent drive
         * letters and conventional paths share one lock.
         */
        for (char& character : key)
        {
            const auto value =
                static_cast<unsigned char>(
                    character
                );

            character =
                static_cast<char>(
                    std::tolower(value)
                );
        }
#endif

        return key;
    }

    void remove_expired_output_mutexes()
    {
        auto iterator =
            output_mutexes_.begin();

        while (
            iterator !=
            output_mutexes_.end()
        )
        {
            if (
                iterator->second.expired()
            )
            {
                iterator =
                    output_mutexes_.erase(
                        iterator
                    );
            }
            else
            {
                ++iterator;
            }
        }
    }

    std::mutex output_mutex_registry_mutex_;

    std::unordered_map<
        std::string,
        std::weak_ptr<std::mutex>
    > output_mutexes_;
};

namespace
{

struct LoadedManifest
{
    std::vector<std::uint8_t> encoded_manifest;
    storage::FileManifest manifest;
};

std::filesystem::path normalized_absolute_path(
    const std::filesystem::path& path
)
{
    std::error_code error;

    std::filesystem::path normalized =
        std::filesystem::absolute(
            path,
            error
        );

    if (error)
    {
        error.clear();

        normalized =
            path;
    }

    normalized =
        normalized.lexically_normal();

    const std::filesystem::path canonical_path =
        std::filesystem::weakly_canonical(
            normalized,
            error
        );

    if (!error)
    {
        normalized =
            canonical_path;
    }

    return normalized;
}

std::string normalized_storage_key(
    const std::filesystem::path& path
)
{
    std::string key =
        normalized_absolute_path(
            path
        ).generic_string();

#ifdef _WIN32
    for (char& character : key)
    {
        const auto value =
            static_cast<unsigned char>(
                character
            );

        character =
            static_cast<char>(
                std::tolower(value)
            );
    }
#endif

    return key;
}

std::shared_ptr<NexusFsServiceConcurrencyState>
acquire_concurrency_state(
    const std::filesystem::path& storage_root
)
{
    static std::mutex registry_mutex;

    static std::unordered_map<
        std::string,
        std::weak_ptr<
            NexusFsServiceConcurrencyState
        >
    > registry;

    const std::string key =
        normalized_storage_key(
            storage_root
        );

    const std::lock_guard lock{
        registry_mutex
    };

    auto iterator =
        registry.begin();

    while (
        iterator !=
        registry.end()
    )
    {
        if (
            iterator->second.expired()
        )
        {
            iterator =
                registry.erase(
                    iterator
                );
        }
        else
        {
            ++iterator;
        }
    }

    const auto existing =
        registry.find(
            key
        );

    if (
        existing !=
        registry.end()
    )
    {
        if (
            const std::shared_ptr<
                NexusFsServiceConcurrencyState
            > state =
                existing->second.lock()
        )
        {
            return state;
        }
    }

    const auto state =
        std::make_shared<
            NexusFsServiceConcurrencyState
        >();

    registry[key] =
        state;

    return state;
}

LoadedManifest load_canonical_manifest(
    const storage::ManifestStore& manifest_store,
    const std::string& manifest_id
)
{
    auto encoded_manifest =
        manifest_store.load(
            manifest_id
        );

    auto manifest =
        storage::FileManifestCodec::decode(
            encoded_manifest
        );

    const auto canonical_manifest =
        storage::FileManifestCodec::encode(
            manifest
        );

    if (
        canonical_manifest !=
        encoded_manifest
    )
    {
        throw std::runtime_error(
            "Stored manifest is not canonically encoded: "
            + manifest_id
        );
    }

    return LoadedManifest{
        std::move(encoded_manifest),
        std::move(manifest)
    };
}

}

NexusFsService::NexusFsService(
    std::filesystem::path storage_root,
    std::size_t default_chunk_size
)
    : NexusFsService{
          std::move(storage_root),
          default_chunk_size,
          nullptr,
          1,
          true,
          nullptr,
          nullptr
      }
{
}

NexusFsService::NexusFsService(
    std::filesystem::path storage_root,
    std::size_t default_chunk_size,
    std::shared_ptr<
        cluster::ClusterNodeFoundation
    > cluster_node,
    std::size_t replication_factor,
    bool strict_replication,
    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry,
    std::shared_ptr<
        observability::JsonLogger
    > logger
)
    : storage_root_{
          std::move(storage_root)
      },
      default_chunk_size_{
          default_chunk_size
      },
      replication_factor_{
          replication_factor
      },
      strict_replication_{
          strict_replication
      }
{
    if (storage_root_.empty())
    {
        throw std::invalid_argument(
            "NexusFS storage root cannot be empty."
        );
    }

    if (default_chunk_size_ == 0)
    {
        throw std::invalid_argument(
            "NexusFS default chunk size must be greater than zero."
        );
    }

    if (replication_factor_ == 0)
    {
        throw std::invalid_argument(
            "NexusFS replication factor must be at least one."
        );
    }

    if (
        replication_factor_ > 1
        && !cluster_node
    )
    {
        throw std::invalid_argument(
            "A cluster node is required when the "
            "replication factor is greater than one."
        );
    }

    if (!metrics_registry)
    {
        metrics_registry =
            std::make_shared<
                observability::MetricsRegistry
            >();
    }

    if (!logger)
    {
        logger =
            std::make_shared<
                observability::JsonLogger
            >();
    }

    if (cluster_node)
    {
        metadata_coordinator_ =
            std::make_shared<
                cluster::MetadataCoordinator
            >(
                cluster_node,
                std::chrono::milliseconds{
                    3000
                },
                metrics_registry,
                logger
            );

        metadata_catalog_synchronizer_ =
            std::make_shared<
                cluster::MetadataCatalogSynchronizer
            >(
                cluster_node,
                std::chrono::milliseconds{
                    3000
                },
                metrics_registry,
                logger
            );

        replica_repair_coordinator_ =
            std::make_shared<
                cluster::ReplicaRepairCoordinator
            >(
                cluster_node,
                std::chrono::milliseconds{
                    3000
                },
                metrics_registry,
                logger
            );

        replica_maintenance_coordinator_ =
            std::make_shared<
                cluster::ReplicaMaintenanceCoordinator
            >(
                cluster_node,
                replication_factor_,
                std::chrono::milliseconds{
                    3000
                },
                metrics_registry,
                logger
            );
    }

    if (replication_factor_ > 1)
    {
        replication_coordinator_ =
            std::make_shared<
                cluster::ReplicationCoordinator
            >(
                std::move(cluster_node),
                std::chrono::milliseconds{
                    3000
                },
                std::move(metrics_registry),
                std::move(logger)
            );
    }

    concurrency_state_ =
        acquire_concurrency_state(
            storage_root_
        );

    /*
     * Initialize the storage layout under the exclusive
     * process-wide lock. Read operations may then construct
     * stores without racing while directories are created.
     */
    const std::unique_lock storage_lock{
        concurrency_state_->
            storage_mutex
    };

    const storage::ChunkStore chunk_store{
        storage_root_
    };

    const storage::ManifestStore manifest_store{
        storage_root_
    };

    (void)chunk_store;
    (void)manifest_store;
}

void NexusFsService::repair_missing_manifest(
    const std::string& manifest_id
) const
{
    if (!metadata_coordinator_)
    {
        return;
    }

    const std::unique_lock storage_lock{
        concurrency_state_->
            storage_mutex
    };

    storage::ManifestStore manifest_store{
        storage_root_
    };

    if (
        manifest_store.contains(
            manifest_id
        )
    )
    {
        (void)load_canonical_manifest(
            manifest_store,
            manifest_id
        );

        return;
    }

    const cluster::ManifestRecoveryReport report =
        metadata_coordinator_->
            recover_manifest(
                manifest_id,
                manifest_store
            );

    if (!report.recovered)
    {
        std::string message =
            "Missing manifest could not be recovered: "
            + manifest_id
            + ". Attempted "
            + std::to_string(
                report.owner_attempts
            )
            + " ordered metadata owners.";

        if (!report.failures.empty())
        {
            message +=
                " First owner failure: "
                + report.failures.front()
                    .peer_node_id
                + " - "
                + report.failures.front()
                    .error;
        }

        throw std::runtime_error(
            message
        );
    }

    (void)load_canonical_manifest(
        manifest_store,
        manifest_id
    );
}

void NexusFsService::
repair_missing_manifest_chunks(
    const std::string& manifest_id
) const
{
    repair_missing_manifest(
        manifest_id
    );

    if (!replica_repair_coordinator_)
    {
        return;
    }

    /*
     * Recovery publishes local chunks, so it uses the exclusive
     * storage lock. Normal complete-file restores continue using
     * the shared lock after this short repair check finishes.
     */
    const std::unique_lock storage_lock{
        concurrency_state_->
            storage_mutex
    };

    storage::ChunkStore chunk_store{
        storage_root_
    };

    const storage::ManifestStore manifest_store{
        storage_root_
    };

    const LoadedManifest loaded_manifest =
        load_canonical_manifest(
            manifest_store,
            manifest_id
        );

    for (
        const std::string& chunk_hash :
        loaded_manifest.manifest
            .chunk_hashes()
    )
    {
        if (
            chunk_store.contains(
                chunk_hash
            )
        )
        {
            continue;
        }

        const cluster::ChunkRecoveryReport report =
            replica_repair_coordinator_->
                recover_chunk(
                    chunk_hash,
                    chunk_store
                );

        if (!report.recovered)
        {
            std::string message =
                "Missing chunk could not be recovered: "
                + chunk_hash
                + ". Attempted "
                + std::to_string(
                    report.peer_attempts
                )
                + " configured peers.";

            if (!report.failures.empty())
            {
                message +=
                    " First peer failure: "
                    + report.failures.front()
                        .peer_node_id
                    + " - "
                    + report.failures.front()
                        .error;
            }

            throw std::runtime_error(
                message
            );
        }
    }
}

StoreFileResult NexusFsService::store_file(
    const std::filesystem::path& source_path
) const
{
    if (source_path.empty())
    {
        throw std::invalid_argument(
            "Source file path cannot be empty."
        );
    }

    const std::unique_lock storage_lock{
        concurrency_state_->
            storage_mutex
    };

    const storage::Chunker chunker{
        default_chunk_size_
    };

    storage::ChunkStore chunk_store{
        storage_root_
    };

    storage::ManifestStore manifest_store{
        storage_root_
    };

    const auto chunks =
        chunker.split_file(
            source_path
        );

    const auto manifest =
        storage::FileManifest::create(
            source_path,
            chunker.chunk_size(),
            chunks
        );

    const auto encoded_manifest =
        storage::FileManifestCodec::encode(
            manifest
        );

    const std::string manifest_id =
        storage::Sha256Hasher::hash(
            encoded_manifest
        );

    std::size_t chunks_stored = 0;
    std::size_t chunks_reused = 0;

    std::size_t remote_replica_acknowledgements = 0;
    std::size_t replication_satisfied_chunks = 0;

    for (
        const auto& chunk :
        chunks
    )
    {
        const storage::StoreResult
            store_result =
                chunk_store.store(
                    chunk
                );

        if (
            store_result ==
            storage::StoreResult::stored
        )
        {
            ++chunks_stored;
        }
        else
        {
            ++chunks_reused;
        }

        const auto loaded_chunk =
            chunk_store.load(
                chunk.hash
            );

        if (
            loaded_chunk !=
            chunk.data
        )
        {
            throw std::runtime_error(
                "Stored chunk data does not match "
                "the source chunk at index "
                + std::to_string(
                    chunk.index
                )
                + "."
            );
        }

        if (replication_coordinator_)
        {
            const cluster::ReplicationReport report =
                replication_coordinator_->
                    replicate_chunk(
                        chunk.hash,
                        chunk.data,
                        replication_factor_
                    );

            remote_replica_acknowledgements +=
                report.acknowledged_replicas;

            if (report.satisfied)
            {
                ++replication_satisfied_chunks;
            }
            else if (strict_replication_)
            {
                std::string message =
                    "Strict chunk replication failed "
                    "for chunk "
                    + chunk.hash
                    + ": acknowledged "
                    + std::to_string(
                        report.acknowledged_replicas
                    )
                    + " of "
                    + std::to_string(
                        report.requested_remote_replicas
                    )
                    + " required remote replicas.";

                if (!report.failures.empty())
                {
                    message +=
                        " First peer failure: "
                        + report.failures.front()
                            .peer_node_id
                        + " - "
                        + report.failures.front()
                            .error;
                }

                throw std::runtime_error(
                    message
                );
            }
        }
        else
        {
            /*
             * Replication factor one is satisfied by the verified
             * local content-addressed copy.
             */
            ++replication_satisfied_chunks;
        }
    }

    bool manifest_stored =
        false;

    std::string metadata_owner_node_id;

    bool metadata_owner_local =
        true;

    bool metadata_owner_acknowledged =
        true;

    if (metadata_coordinator_)
    {
        const cluster::MetadataPublicationReport
            metadata_report =
                metadata_coordinator_->
                    publish_manifest(
                        manifest_id,
                        encoded_manifest,
                        manifest_store
                    );

        manifest_stored =
            metadata_report
                .local_cache_created;

        metadata_owner_node_id =
            metadata_report
                .owner
                .node_id;

        metadata_owner_local =
            metadata_report
                .owner
                .local;

        metadata_owner_acknowledged =
            metadata_report
                .owner_acknowledged;
    }
    else
    {
        const storage::ManifestStoreResult
            manifest_store_result =
                manifest_store.store(
                    manifest_id,
                    encoded_manifest
                );

        manifest_stored =
            manifest_store_result ==
            storage::ManifestStoreResult::stored;
    }

    const LoadedManifest loaded_manifest =
        load_canonical_manifest(
            manifest_store,
            manifest_id
        );

    if (
        loaded_manifest.encoded_manifest !=
        encoded_manifest
    )
    {
        throw std::runtime_error(
            "Stored manifest bytes do not match "
            "the encoded source manifest."
        );
    }

    const bool replication_satisfied =
        replication_satisfied_chunks ==
        chunks.size();

    return StoreFileResult{
        source_path,
        manifest_id,
        manifest.original_filename(),
        manifest.file_size(),
        manifest.chunk_count(),
        chunks_stored,
        chunks_reused,
        manifest.file_size(),
        encoded_manifest.size(),
        manifest_stored,
        replication_factor_,
        remote_replica_acknowledgements,
        replication_satisfied,
        metadata_owner_node_id,
        metadata_owner_local,
        metadata_owner_acknowledged
    };
}

RestoreFileResult NexusFsService::restore_file(
    const std::string& manifest_id,
    const std::filesystem::path& output_path
) const
{
    if (manifest_id.empty())
    {
        throw std::invalid_argument(
            "Restore manifest ID cannot be empty."
        );
    }

    if (output_path.empty())
    {
        throw std::invalid_argument(
            "Restore output path cannot be empty."
        );
    }

    const std::shared_ptr<std::mutex>
        output_mutex =
            concurrency_state_->
                output_mutex_for(
                    output_path
                );

    /*
     * The output lock serializes restorations targeting the
     * same normalized path while allowing unrelated output
     * files to be reconstructed concurrently.
     */
    const std::unique_lock output_lock{
        *output_mutex
    };

    if (replica_repair_coordinator_)
    {
        repair_missing_manifest_chunks(
            manifest_id
        );
    }

    const std::shared_lock storage_lock{
        concurrency_state_->
            storage_mutex
    };

    const storage::ChunkStore chunk_store{
        storage_root_
    };

    const storage::ManifestStore manifest_store{
        storage_root_
    };

    const LoadedManifest loaded_manifest =
        load_canonical_manifest(
            manifest_store,
            manifest_id
        );

    const storage::ReconstructionResult
        reconstruction_result =
            storage::FileReconstructor::reconstruct(
                loaded_manifest.manifest,
                chunk_store,
                output_path
            );

    return RestoreFileResult{
        manifest_id,
        loaded_manifest.manifest
            .original_filename(),
        loaded_manifest.manifest.file_size(),
        loaded_manifest.manifest.chunk_count(),
        output_path,
        reconstruction_result.bytes_written,
        reconstruction_result.chunks_loaded
    };
}

InspectFileResult NexusFsService::inspect_file(
    const std::string& manifest_id
) const
{
    if (manifest_id.empty())
    {
        throw std::invalid_argument(
            "Inspect manifest ID cannot be empty."
        );
    }

    if (metadata_coordinator_)
    {
        repair_missing_manifest(
            manifest_id
        );
    }

    const std::shared_lock storage_lock{
        concurrency_state_->
            storage_mutex
    };

    const storage::ChunkStore chunk_store{
        storage_root_
    };

    const storage::ManifestStore manifest_store{
        storage_root_
    };

    const LoadedManifest loaded_manifest =
        load_canonical_manifest(
            manifest_store,
            manifest_id
        );

    std::vector<InspectedChunk>
        inspected_chunks;

    inspected_chunks.reserve(
        loaded_manifest.manifest
            .chunk_count()
    );

    std::size_t available_chunks = 0;
    std::size_t missing_chunks = 0;

    const auto& chunk_hashes =
        loaded_manifest.manifest
            .chunk_hashes();

    for (
        std::size_t index = 0;
        index < chunk_hashes.size();
        ++index
    )
    {
        const std::string& chunk_hash =
            chunk_hashes[index];

        const bool is_present =
            chunk_store.contains(
                chunk_hash
            );

        if (is_present)
        {
            ++available_chunks;
        }
        else
        {
            ++missing_chunks;
        }

        inspected_chunks.push_back(
            InspectedChunk{
                index,
                chunk_hash,
                is_present
            }
        );
    }

    return InspectFileResult{
        manifest_id,
        loaded_manifest.manifest
            .original_filename(),
        loaded_manifest.manifest.file_size(),
        loaded_manifest.manifest.chunk_size(),
        loaded_manifest.encoded_manifest.size(),
        std::move(inspected_chunks),
        available_chunks,
        missing_chunks
    };
}

VerifyFileResult NexusFsService::verify_file(
    const std::string& manifest_id
) const
{
    if (manifest_id.empty())
    {
        throw std::invalid_argument(
            "Verify manifest ID cannot be empty."
        );
    }

    if (replica_repair_coordinator_)
    {
        repair_missing_manifest_chunks(
            manifest_id
        );
    }

    const std::shared_lock storage_lock{
        concurrency_state_->
            storage_mutex
    };

    const storage::ChunkStore chunk_store{
        storage_root_
    };

    const storage::ManifestStore manifest_store{
        storage_root_
    };

    const LoadedManifest loaded_manifest =
        load_canonical_manifest(
            manifest_store,
            manifest_id
        );

    const storage::FileVerificationResult
        verification_result =
            storage::FileVerifier::verify(
                loaded_manifest.manifest,
                chunk_store
            );

    std::vector<VerifiedChunkResult>
        verified_chunks;

    verified_chunks.reserve(
        verification_result
            .verified_chunks.size()
    );

    for (
        const storage::VerifiedChunk&
            verified_chunk :
        verification_result
            .verified_chunks
    )
    {
        verified_chunks.push_back(
            VerifiedChunkResult{
                verified_chunk.index,
                verified_chunk.hash,
                verified_chunk.bytes_verified
            }
        );
    }

    return VerifyFileResult{
        manifest_id,
        loaded_manifest.manifest
            .original_filename(),
        loaded_manifest.manifest.file_size(),
        loaded_manifest.manifest.chunk_count(),
        std::move(verified_chunks),
        verification_result
            .total_bytes_verified
    };
}

ListFilesResult NexusFsService::list_files() const
{
    const std::shared_lock storage_lock{
        concurrency_state_->
            storage_mutex
    };

    const storage::ChunkStore chunk_store{
        storage_root_
    };

    const storage::ManifestStore manifest_store{
        storage_root_
    };

    const std::vector<std::string>
        manifest_ids =
            manifest_store
                .list_manifest_ids();

    std::vector<StoredFileSummary> files;

    files.reserve(
        manifest_ids.size()
    );

    std::size_t complete_manifests = 0;
    std::size_t incomplete_manifests = 0;

    for (
        const std::string& manifest_id :
        manifest_ids
    )
    {
        const LoadedManifest loaded_manifest =
            load_canonical_manifest(
                manifest_store,
                manifest_id
            );

        std::size_t missing_chunks = 0;

        for (
            const std::string& chunk_hash :
            loaded_manifest.manifest
                .chunk_hashes()
        )
        {
            if (
                !chunk_store.contains(
                    chunk_hash
                )
            )
            {
                ++missing_chunks;
            }
        }

        if (missing_chunks == 0)
        {
            ++complete_manifests;
        }
        else
        {
            ++incomplete_manifests;
        }

        files.push_back(
            StoredFileSummary{
                manifest_id,
                loaded_manifest.manifest
                    .original_filename(),
                loaded_manifest.manifest
                    .file_size(),
                loaded_manifest.manifest
                    .chunk_size(),
                loaded_manifest.manifest
                    .chunk_count(),
                missing_chunks
            }
        );
    }

    return ListFilesResult{
        std::move(files),
        complete_manifests,
        incomplete_manifests
    };
}

SynchronizeMetadataCatalogResult
NexusFsService::synchronize_metadata_catalog() const
{
    if (!metadata_catalog_synchronizer_)
    {
        throw std::runtime_error(
            "Metadata catalog synchronization requires "
            "cluster services."
        );
    }

    /*
     * Synchronization may durably publish recovered manifests.
     * The entire additive operation therefore uses the exclusive
     * process-wide storage lock.
     */
    const std::unique_lock storage_lock{
        concurrency_state_->
            storage_mutex
    };

    storage::ManifestStore manifest_store{
        storage_root_
    };

    const storage::ChunkStore chunk_store{
        storage_root_
    };

    const cluster::MetadataCatalogSyncReport report =
        metadata_catalog_synchronizer_->
            synchronize(
                manifest_store
            );

    std::vector<StoredFileSummary> files;

    files.reserve(
        report.synchronized_entries.size()
    );

    for (
        const cluster::MetadataCatalogEntry& entry :
        report.synchronized_entries
    )
    {
        const LoadedManifest loaded_manifest =
            load_canonical_manifest(
                manifest_store,
                entry.manifest_id
            );

        std::size_t missing_chunks = 0;

        for (
            const std::string& chunk_hash :
            loaded_manifest.manifest
                .chunk_hashes()
        )
        {
            if (
                !chunk_store.contains(
                    chunk_hash
                )
            )
            {
                ++missing_chunks;
            }
        }

        files.push_back(
            StoredFileSummary{
                entry.manifest_id,
                entry.original_filename,
                entry.file_size,
                static_cast<std::size_t>(
                    entry.chunk_size
                ),
                static_cast<std::size_t>(
                    entry.chunk_count
                ),
                missing_chunks
            }
        );
    }

    return SynchronizeMetadataCatalogResult{
        std::move(files),
        report.peers_contacted,
        report.peers_succeeded,
        report.peers_failed,
        report.remote_entries_observed,
        report.unique_entries_discovered,
        report.manifests_already_local,
        report.manifests_recovered,
        report.manifests_unrecovered,
        report.conflicts_detected,
        report.converged
    };
}

RepairReplicasResult
NexusFsService::repair_replicas() const
{
    if (!replica_maintenance_coordinator_)
    {
        return RepairReplicasResult{
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            true
        };
    }

    /*
     * Maintenance may durably publish missing local chunks, so the
     * sweep uses the exclusive process-wide storage lock.
     */
    const std::unique_lock storage_lock{
        concurrency_state_->
            storage_mutex
    };

    storage::ChunkStore chunk_store{
        storage_root_
    };

    const storage::ManifestStore manifest_store{
        storage_root_
    };

    const std::vector<std::string>
        manifest_ids =
            manifest_store
                .list_manifest_ids();

    std::unordered_set<std::string>
        unique_chunk_hashes;

    for (
        const std::string& manifest_id :
        manifest_ids
    )
    {
        const LoadedManifest loaded_manifest =
            load_canonical_manifest(
                manifest_store,
                manifest_id
            );

        for (
            const std::string& chunk_hash :
            loaded_manifest.manifest
                .chunk_hashes()
        )
        {
            unique_chunk_hashes.insert(
                chunk_hash
            );
        }
    }

    const std::vector<std::string>
        chunk_hashes{
            unique_chunk_hashes.begin(),
            unique_chunk_hashes.end()
        };

    const cluster::ReplicaMaintenanceReport
        report =
            replica_maintenance_coordinator_->
                repair_chunks(
                    chunk_hashes,
                    chunk_store
                );

    return RepairReplicasResult{
        manifest_ids.size(),
        report.chunks_scanned,
        report.local_chunks_recovered,
        report.remote_replicas_observed,
        report.remote_replicas_created,
        report.peer_failures,
        report.under_replicated_chunks,
        report.fully_repaired
    };
}

const std::filesystem::path&
NexusFsService::storage_root() const noexcept
{
    return storage_root_;
}

std::size_t
NexusFsService::default_chunk_size() const noexcept
{
    return default_chunk_size_;
}

}