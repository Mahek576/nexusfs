#include "nexusfs/cluster/replica_maintenance.hpp"

#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/chunk_store.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nexusfs::cluster
{

ReplicaMaintenanceCoordinator::
ReplicaMaintenanceCoordinator(
    std::shared_ptr<
        ClusterNodeFoundation
    > cluster_node,
    std::size_t replication_factor,
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
      replication_factor_{
          replication_factor
      },
      transport_{
          cluster_node_,
          timeout
      },
      recovery_coordinator_{
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
            "Replica-maintenance cluster node "
            "cannot be null."
        );
    }

    if (replication_factor_ == 0)
    {
        throw std::invalid_argument(
            "Replica-maintenance replication factor "
            "must be at least one."
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

ReplicaMaintenanceReport
ReplicaMaintenanceCoordinator::repair_chunks(
    const std::vector<std::string>& chunk_hashes,
    storage::ChunkStore& local_chunk_store
)
{
    ReplicaMaintenanceReport report;

    std::unordered_set<std::string>
        unique_hashes;

    unique_hashes.reserve(
        chunk_hashes.size()
    );

    for (
        const std::string& chunk_hash :
        chunk_hashes
    )
    {
        unique_hashes.insert(
            chunk_hash
        );
    }

    const std::size_t
        required_remote_replicas =
            replication_factor_ - 1;

    for (
        const std::string& chunk_hash :
        unique_hashes
    )
    {
        ++report.chunks_scanned;

        if (
            !local_chunk_store.contains(
                chunk_hash
            )
        )
        {
            const ChunkRecoveryReport
                recovery_report =
                    recovery_coordinator_.
                        recover_chunk(
                            chunk_hash,
                            local_chunk_store
                        );

            report.peer_failures +=
                recovery_report
                    .failures
                    .size();

            if (!recovery_report.recovered)
            {
                ++report
                    .under_replicated_chunks;

                continue;
            }

            ++report.local_chunks_recovered;
        }

        /*
         * load() validates the local chunk hash before the bytes are
         * used to create replacement replicas.
         */
        const std::vector<std::uint8_t>
            local_data =
                local_chunk_store.load(
                    chunk_hash
                );

        if (required_remote_replicas == 0)
        {
            continue;
        }

        const std::vector<PeerDefinition>
            ordered_peers =
                ReplicationCoordinator::
                    select_replica_peers(
                        chunk_hash,
                        cluster_node_
                            ->configuration()
                            .peers,
                        cluster_node_
                            ->configuration()
                            .peers.size()
                    );

        std::size_t confirmed_remote_replicas =
            0;

        for (
            const PeerDefinition& peer :
            ordered_peers
        )
        {
            try
            {
                if (
                    transport_.chunk_exists(
                        peer,
                        chunk_hash
                    )
                )
                {
                    ++report
                        .remote_replicas_observed;

                    ++confirmed_remote_replicas;
                }
                else
                {
                    const RemoteChunkStoreResult
                        store_result =
                            transport_.store_chunk(
                                peer,
                                chunk_hash,
                                local_data
                            );

                    if (
                        store_result ==
                        RemoteChunkStoreResult::stored
                    )
                    {
                        ++report
                            .remote_replicas_created;
                    }
                    else
                    {
                        ++report
                            .remote_replicas_observed;
                    }

                    ++confirmed_remote_replicas;
                }
            }
            catch (const std::exception&)
            {
                ++report.peer_failures;
            }

            if (
                confirmed_remote_replicas >=
                required_remote_replicas
            )
            {
                break;
            }
        }

        if (
            confirmed_remote_replicas <
            required_remote_replicas
        )
        {
            ++report.under_replicated_chunks;
        }
    }

    report.fully_repaired =
        report.under_replicated_chunks == 0;

    metrics_registry_->
        record_replica_maintenance(
            static_cast<std::uint64_t>(
                report.chunks_scanned
            ),
            static_cast<std::uint64_t>(
                report.local_chunks_recovered
            ),
            static_cast<std::uint64_t>(
                report.remote_replicas_observed
            ),
            static_cast<std::uint64_t>(
                report.remote_replicas_created
            ),
            static_cast<std::uint64_t>(
                report.peer_failures
            ),
            static_cast<std::uint64_t>(
                report.under_replicated_chunks
            )
        );

    logger_->log(
        report.fully_repaired
            ? observability::LogLevel::info
            : observability::LogLevel::warning,
        "replica_maintenance_completed",
        report.fully_repaired
            ? "Replica maintenance completed successfully."
            : "Replica maintenance completed with "
              "under-replicated chunks.",
        {
            observability::LogField{
                "chunks_scanned",
                static_cast<std::uint64_t>(
                    report.chunks_scanned
                )
            },
            observability::LogField{
                "local_chunks_recovered",
                static_cast<std::uint64_t>(
                    report.local_chunks_recovered
                )
            },
            observability::LogField{
                "remote_replicas_observed",
                static_cast<std::uint64_t>(
                    report.remote_replicas_observed
                )
            },
            observability::LogField{
                "remote_replicas_created",
                static_cast<std::uint64_t>(
                    report.remote_replicas_created
                )
            },
            observability::LogField{
                "peer_failures",
                static_cast<std::uint64_t>(
                    report.peer_failures
                )
            },
            observability::LogField{
                "under_replicated_chunks",
                static_cast<std::uint64_t>(
                    report.under_replicated_chunks
                )
            },
            observability::LogField{
                "fully_repaired",
                report.fully_repaired
            }
        }
    );

    return report;
}

}
