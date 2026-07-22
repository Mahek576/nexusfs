#include "nexusfs/cluster/metadata_catalog_synchronizer.hpp"

#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/manifest_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nexusfs::cluster
{

namespace
{

MetadataCatalogEntry entry_from_manifest(
    const std::string& manifest_id,
    const std::vector<std::uint8_t>& encoded_manifest
)
{
    const storage::FileManifest manifest =
        storage::FileManifestCodec::decode(
            encoded_manifest
        );

    const std::vector<std::uint8_t> canonical =
        storage::FileManifestCodec::encode(
            manifest
        );

    if (canonical != encoded_manifest)
    {
        throw std::runtime_error(
            "Local catalog manifest is not canonically encoded: "
            + manifest_id
        );
    }

    MetadataCatalogEntry entry{
        manifest_id,
        manifest.original_filename(),
        manifest.file_size(),
        static_cast<std::uint64_t>(
            manifest.chunk_size()
        ),
        static_cast<std::uint64_t>(
            manifest.chunk_count()
        )
    };

    if (
        !MetadataCatalogCodec::entry_matches_manifest(
            entry,
            encoded_manifest
        )
    )
    {
        throw std::runtime_error(
            "Local catalog entry does not match manifest: "
            + manifest_id
        );
    }

    return entry;
}

MetadataCatalogSnapshot local_snapshot(
    const ClusterNodeFoundation& cluster_node,
    const storage::ManifestStore& manifest_store
)
{
    const std::vector<std::string> manifest_ids =
        manifest_store.list_manifest_ids();

    std::vector<MetadataCatalogEntry> entries;

    entries.reserve(
        manifest_ids.size()
    );

    for (
        const std::string& manifest_id :
        manifest_ids
    )
    {
        entries.push_back(
            entry_from_manifest(
                manifest_id,
                manifest_store.load(
                    manifest_id
                )
            )
        );
    }

    return MetadataCatalogCodec::create(
        cluster_node.identity().node_id,
        std::move(entries)
    );
}

}

MetadataCatalogSynchronizer::
MetadataCatalogSynchronizer(
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
      metadata_coordinator_{
          cluster_node_,
          timeout,
          metrics_registry,
          logger
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
            "Metadata catalog synchronizer cluster node "
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

MetadataCatalogMergeResult
MetadataCatalogSynchronizer::merge_snapshots(
    std::vector<MetadataCatalogSnapshot> snapshots
)
{
    std::sort(
        snapshots.begin(),
        snapshots.end(),
        [](
            const MetadataCatalogSnapshot& left,
            const MetadataCatalogSnapshot& right
        )
        {
            return left.node_id < right.node_id;
        }
    );

    struct SelectedEntry
    {
        std::string node_id;
        MetadataCatalogEntry entry;
    };

    std::map<std::string, SelectedEntry>
        selected_entries;

    std::vector<MetadataCatalogConflict>
        conflicts;

    for (
        const MetadataCatalogSnapshot& snapshot :
        snapshots
    )
    {
        const MetadataCatalogSnapshot canonical =
            MetadataCatalogCodec::create(
                snapshot.node_id,
                snapshot.entries
            );

        if (
            canonical.digest !=
            snapshot.digest
        )
        {
            throw std::invalid_argument(
                "Metadata catalog snapshot digest is invalid "
                "during synchronization."
            );
        }

        for (
            const MetadataCatalogEntry& entry :
            canonical.entries
        )
        {
            const auto existing =
                selected_entries.find(
                    entry.manifest_id
                );

            if (
                existing ==
                selected_entries.end()
            )
            {
                selected_entries.emplace(
                    entry.manifest_id,
                    SelectedEntry{
                        snapshot.node_id,
                        entry
                    }
                );

                continue;
            }

            if (
                existing->second.entry !=
                entry
            )
            {
                conflicts.push_back(
                    MetadataCatalogConflict{
                        entry.manifest_id,
                        existing->second.node_id,
                        snapshot.node_id,
                        existing->second.entry,
                        entry
                    }
                );
            }
        }
    }

    MetadataCatalogMergeResult result;

    result.entries.reserve(
        selected_entries.size()
    );

    for (
        const auto& [
            manifest_id,
            selected
        ] :
        selected_entries
    )
    {
        (void)manifest_id;

        result.entries.push_back(
            selected.entry
        );
    }

    result.conflicts =
        std::move(conflicts);

    return result;
}

MetadataCatalogSyncReport
MetadataCatalogSynchronizer::synchronize(
    storage::ManifestStore& local_manifest_store
)
{
    MetadataCatalogSyncReport report;

    std::vector<MetadataCatalogSnapshot>
        snapshots;

    snapshots.push_back(
        local_snapshot(
            *cluster_node_,
            local_manifest_store
        )
    );

    std::vector<PeerDefinition> peers =
        cluster_node_
            ->configuration()
            .peers;

    std::sort(
        peers.begin(),
        peers.end(),
        [](
            const PeerDefinition& left,
            const PeerDefinition& right
        )
        {
            return left.node_id < right.node_id;
        }
    );

    for (
        const PeerDefinition& peer :
        peers
    )
    {
        ++report.peers_contacted;

        try
        {
            MetadataCatalogSnapshot snapshot =
                transport_.load_catalog(
                    peer
                );

            report.remote_entries_observed +=
                snapshot.entries.size();

            snapshots.push_back(
                std::move(snapshot)
            );

            ++report.peers_succeeded;
        }
        catch (const std::exception& error)
        {
            ++report.peers_failed;

            report.peer_failures.push_back(
                ReplicationFailure{
                    peer.node_id,
                    error.what()
                }
            );
        }
    }

    const MetadataCatalogMergeResult merged =
        merge_snapshots(
            std::move(snapshots)
        );

    report.unique_entries_discovered =
        merged.entries.size();

    report.conflicts =
        merged.conflicts;

    report.conflicts_detected =
        merged.conflicts.size();

    std::unordered_set<std::string>
        conflicted_manifest_ids;

    conflicted_manifest_ids.reserve(
        merged.conflicts.size()
    );

    for (
        const MetadataCatalogConflict& conflict :
        merged.conflicts
    )
    {
        conflicted_manifest_ids.insert(
            conflict.manifest_id
        );
    }

    for (
        const MetadataCatalogEntry& entry :
        merged.entries
    )
    {
        if (
            conflicted_manifest_ids.contains(
                entry.manifest_id
            )
        )
        {
            continue;
        }

        if (
            local_manifest_store.contains(
                entry.manifest_id
            )
        )
        {
            const std::vector<std::uint8_t>
                encoded_manifest =
                    local_manifest_store.load(
                        entry.manifest_id
                    );

            if (
                !MetadataCatalogCodec::
                    entry_matches_manifest(
                        entry,
                        encoded_manifest
                    )
            )
            {
                ++report.conflicts_detected;

                report.conflicts.push_back(
                    MetadataCatalogConflict{
                        entry.manifest_id,
                        cluster_node_
                            ->identity()
                            .node_id,
                        "catalog_merge",
                        entry_from_manifest(
                            entry.manifest_id,
                            encoded_manifest
                        ),
                        entry
                    }
                );

                continue;
            }

            ++report.manifests_already_local;

            report.synchronized_entries.push_back(
                entry
            );

            continue;
        }

        const ManifestRecoveryReport recovery =
            metadata_coordinator_.
                recover_manifest(
                    entry.manifest_id,
                    local_manifest_store
                );

        if (!recovery.recovered)
        {
            ++report.manifests_unrecovered;

            continue;
        }

        const std::vector<std::uint8_t>
            recovered_manifest =
                local_manifest_store.load(
                    entry.manifest_id
                );

        if (
            !MetadataCatalogCodec::
                entry_matches_manifest(
                    entry,
                    recovered_manifest
                )
        )
        {
            ++report.conflicts_detected;

            report.conflicts.push_back(
                MetadataCatalogConflict{
                    entry.manifest_id,
                    recovery.source_owner_node_id,
                    "catalog_entry",
                    entry_from_manifest(
                        entry.manifest_id,
                        recovered_manifest
                    ),
                    entry
                }
            );

            continue;
        }

        ++report.manifests_recovered;

        report.synchronized_entries.push_back(
            entry
        );
    }

    std::sort(
        report.synchronized_entries.begin(),
        report.synchronized_entries.end(),
        [](
            const MetadataCatalogEntry& left,
            const MetadataCatalogEntry& right
        )
        {
            return left.manifest_id < right.manifest_id;
        }
    );

    report.converged =
        report.peers_failed == 0
        && report.conflicts_detected == 0
        && report.manifests_unrecovered == 0
        && report.synchronized_entries.size() ==
            report.unique_entries_discovered;

    metrics_registry_->
        record_metadata_catalog_sync(
            static_cast<std::uint64_t>(
                report.peers_contacted
            ),
            static_cast<std::uint64_t>(
                report.peers_succeeded
            ),
            static_cast<std::uint64_t>(
                report.remote_entries_observed
            ),
            static_cast<std::uint64_t>(
                report.manifests_recovered
            ),
            static_cast<std::uint64_t>(
                report.manifests_unrecovered
            ),
            static_cast<std::uint64_t>(
                report.conflicts_detected
            ),
            report.converged
        );

    logger_->log(
        report.converged
            ? observability::LogLevel::info
            : observability::LogLevel::warning,
        "metadata_catalog_sync_completed",
        report.converged
            ? "Cluster metadata catalog synchronized successfully."
            : "Cluster metadata catalog synchronization completed "
              "without full convergence.",
        {
            observability::LogField{
                "peers_contacted",
                static_cast<std::uint64_t>(
                    report.peers_contacted
                )
            },
            observability::LogField{
                "peers_succeeded",
                static_cast<std::uint64_t>(
                    report.peers_succeeded
                )
            },
            observability::LogField{
                "peers_failed",
                static_cast<std::uint64_t>(
                    report.peers_failed
                )
            },
            observability::LogField{
                "unique_entries_discovered",
                static_cast<std::uint64_t>(
                    report.unique_entries_discovered
                )
            },
            observability::LogField{
                "manifests_recovered",
                static_cast<std::uint64_t>(
                    report.manifests_recovered
                )
            },
            observability::LogField{
                "manifests_unrecovered",
                static_cast<std::uint64_t>(
                    report.manifests_unrecovered
                )
            },
            observability::LogField{
                "conflicts_detected",
                static_cast<std::uint64_t>(
                    report.conflicts_detected
                )
            },
            observability::LogField{
                "converged",
                report.converged
            }
        }
    );

    return report;
}

}
