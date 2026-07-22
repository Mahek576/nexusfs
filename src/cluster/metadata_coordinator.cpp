#include "nexusfs/cluster/metadata_coordinator.hpp"

#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/manifest_store.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nexusfs::cluster
{

namespace
{

void validate_canonical_manifest(
    const std::string& manifest_id,
    const std::vector<std::uint8_t>& encoded_manifest
)
{
    if (encoded_manifest.empty())
    {
        throw std::invalid_argument(
            "Encoded metadata manifest cannot be empty."
        );
    }

    const std::string calculated_id =
        storage::Sha256Hasher::hash(
            std::span<const std::uint8_t>{
                encoded_manifest.data(),
                encoded_manifest.size()
            }
        );

    if (calculated_id != manifest_id)
    {
        throw std::invalid_argument(
            "Encoded metadata manifest does not match "
            "its manifest ID."
        );
    }

    const storage::FileManifest decoded =
        storage::FileManifestCodec::decode(
            encoded_manifest
        );

    const std::vector<std::uint8_t> canonical =
        storage::FileManifestCodec::encode(
            decoded
        );

    if (canonical != encoded_manifest)
    {
        throw std::invalid_argument(
            "Metadata manifest is not canonically encoded."
        );
    }
}

PeerDefinition peer_for_owner(
    const MetadataOwner& owner
)
{
    return PeerDefinition{
        owner.node_id,
        owner.address,
        owner.port
    };
}

}

MetadataCoordinator::MetadataCoordinator(
    std::shared_ptr<
        ClusterNodeFoundation
    > cluster_node,
    std::chrono::milliseconds timeout,
    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry,
    std::shared_ptr<
        observability::JsonLogger
    > logger
)
    : cluster_node_{
          std::move(cluster_node)
      },
      transport_{
          cluster_node_,
          timeout
      },
      metrics_registry_{
          std::move(metrics_registry)
      },
      logger_{
          std::move(logger)
      }
{
    if (!cluster_node_)
    {
        throw std::invalid_argument(
            "Metadata coordinator cluster node "
            "cannot be null."
        );
    }

    if (!metrics_registry_)
    {
        metrics_registry_ =
            std::make_shared<
                observability::MetricsRegistry
            >();
    }

    if (!logger_)
    {
        logger_ =
            std::make_shared<
                observability::JsonLogger
            >();
    }
}

MetadataPublicationReport
MetadataCoordinator::publish_manifest(
    const std::string& manifest_id,
    const std::vector<std::uint8_t>& encoded_manifest,
    storage::ManifestStore& local_manifest_store
)
{
    validate_canonical_manifest(
        manifest_id,
        encoded_manifest
    );

    MetadataPublicationReport report;

    report.owner =
        MetadataOwnership::select_owner(
            manifest_id,
            cluster_node_->identity(),
            cluster_node_->configuration()
        );

    try
    {
        if (report.owner.local)
        {
            const storage::ManifestStoreResult
                local_result =
                    local_manifest_store.store(
                        manifest_id,
                        encoded_manifest
                    );

            report.owner_created =
                local_result ==
                storage::ManifestStoreResult::stored;

            report.local_cache_created =
                report.owner_created;

            report.owner_acknowledged =
                true;
        }
        else
        {
            const RemoteManifestStoreResult
                owner_result =
                    transport_.store_manifest(
                        peer_for_owner(
                            report.owner
                        ),
                        manifest_id,
                        encoded_manifest
                    );

            report.owner_created =
                owner_result ==
                RemoteManifestStoreResult::stored;

            /*
             * The remote owner has durably acknowledged the
             * content-addressed metadata before the local cache is
             * published.
             */
            report.owner_acknowledged =
                true;

            const storage::ManifestStoreResult
                cache_result =
                    local_manifest_store.store(
                        manifest_id,
                        encoded_manifest
                    );

            report.local_cache_created =
                cache_result ==
                storage::ManifestStoreResult::stored;
        }

        const std::vector<std::uint8_t>
            locally_loaded =
                local_manifest_store.load(
                    manifest_id
                );

        validate_canonical_manifest(
            manifest_id,
            locally_loaded
        );

        if (
            locally_loaded !=
            encoded_manifest
        )
        {
            throw std::runtime_error(
                "Local metadata cache does not match "
                "the owner-acknowledged manifest."
            );
        }

        metrics_registry_->
            record_metadata_publication(
                true
            );

        logger_->log(
            observability::LogLevel::info,
            "metadata_publication_succeeded",
            "Manifest metadata was acknowledged by "
            "its deterministic owner.",
            {
                observability::LogField{
                    "manifest_id",
                    manifest_id
                },
                observability::LogField{
                    "owner_node_id",
                    report.owner.node_id
                },
                observability::LogField{
                    "owner_local",
                    report.owner.local
                },
                observability::LogField{
                    "owner_created",
                    report.owner_created
                },
                observability::LogField{
                    "local_cache_created",
                    report.local_cache_created
                }
            }
        );

        return report;
    }
    catch (const std::exception& error)
    {
        metrics_registry_->
            record_metadata_publication(
                false
            );

        logger_->log(
            observability::LogLevel::error,
            "metadata_publication_failed",
            "Manifest metadata was not acknowledged "
            "by its deterministic owner.",
            {
                observability::LogField{
                    "manifest_id",
                    manifest_id
                },
                observability::LogField{
                    "owner_node_id",
                    report.owner.node_id
                },
                observability::LogField{
                    "owner_local",
                    report.owner.local
                },
                observability::LogField{
                    "error",
                    error.what()
                }
            }
        );

        throw;
    }
}

ManifestRecoveryReport
MetadataCoordinator::recover_manifest(
    const std::string& manifest_id,
    storage::ManifestStore& local_manifest_store
)
{
    ManifestRecoveryReport report;

    if (
        local_manifest_store.contains(
            manifest_id
        )
    )
    {
        const std::vector<std::uint8_t>
            encoded_manifest =
                local_manifest_store.load(
                    manifest_id
                );

        validate_canonical_manifest(
            manifest_id,
            encoded_manifest
        );

        report.already_local =
            true;

        report.recovered =
            true;

        return report;
    }

    const std::vector<MetadataOwner> owners =
        MetadataOwnership::ordered_owners(
            manifest_id,
            cluster_node_->identity(),
            cluster_node_->configuration()
        );

    for (
        const MetadataOwner& owner :
        owners
    )
    {
        if (owner.local)
        {
            continue;
        }

        ++report.owner_attempts;

        try
        {
            const std::vector<std::uint8_t>
                encoded_manifest =
                    transport_.load_manifest(
                        peer_for_owner(
                            owner
                        ),
                        manifest_id
                    );

            validate_canonical_manifest(
                manifest_id,
                encoded_manifest
            );

            (void)local_manifest_store.store(
                manifest_id,
                encoded_manifest
            );

            const std::vector<std::uint8_t>
                published_manifest =
                    local_manifest_store.load(
                        manifest_id
                    );

            validate_canonical_manifest(
                manifest_id,
                published_manifest
            );

            if (
                published_manifest !=
                encoded_manifest
            )
            {
                throw std::runtime_error(
                    "Recovered local metadata does not match "
                    "the verified remote manifest."
                );
            }

            metrics_registry_->
                record_remote_manifest_read(
                    true
                );

            metrics_registry_->
                record_local_manifest_repair();

            logger_->log(
                observability::LogLevel::info,
                "metadata_recovery_succeeded",
                "A missing local manifest was recovered "
                "from an ordered metadata owner.",
                {
                    observability::LogField{
                        "manifest_id",
                        manifest_id
                    },
                    observability::LogField{
                        "source_owner_node_id",
                        owner.node_id
                    },
                    observability::LogField{
                        "owner_attempts",
                        static_cast<std::uint64_t>(
                            report.owner_attempts
                        )
                    },
                    observability::LogField{
                        "encoded_bytes",
                        static_cast<std::uint64_t>(
                            encoded_manifest.size()
                        )
                    }
                }
            );

            report.recovered =
                true;

            report.source_owner_node_id =
                owner.node_id;

            return report;
        }
        catch (const std::exception& error)
        {
            metrics_registry_->
                record_remote_manifest_read(
                    false
                );

            report.failures.push_back(
                ReplicationFailure{
                    owner.node_id,
                    error.what()
                }
            );
        }
    }

    logger_->log(
        observability::LogLevel::error,
        "metadata_recovery_failed",
        "A missing local manifest could not be "
        "recovered from any ordered metadata owner.",
        {
            observability::LogField{
                "manifest_id",
                manifest_id
            },
            observability::LogField{
                "owner_attempts",
                static_cast<std::uint64_t>(
                    report.owner_attempts
                )
            },
            observability::LogField{
                "owner_failures",
                static_cast<std::uint64_t>(
                    report.failures.size()
                )
            }
        }
    );

    return report;
}

}
