#include "nexusfs/cluster/replica_repair.hpp"

#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/chunker.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nexusfs::cluster
{

ReplicaRepairCoordinator::
ReplicaRepairCoordinator(
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
            "Replica-repair cluster node "
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

ChunkRecoveryReport
ReplicaRepairCoordinator::recover_chunk(
    const std::string& chunk_hash,
    storage::ChunkStore& local_chunk_store
)
{
    ChunkRecoveryReport report;

    if (
        local_chunk_store.contains(
            chunk_hash
        )
    )
    {
        /*
         * Loading verifies the local content-addressed object.
         */
        (void)local_chunk_store.load(
            chunk_hash
        );

        report.already_local =
            true;

        report.recovered =
            true;

        return report;
    }

    const std::vector<PeerDefinition>
        selected_peers =
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

    for (
        const PeerDefinition& peer :
        selected_peers
    )
    {
        ++report.peer_attempts;

        try
        {
            const std::vector<std::uint8_t>
                recovered_data =
                    transport_.load_chunk(
                        peer,
                        chunk_hash
                    );

            metrics_registry_->
                record_remote_chunk_read(
                    true
                );

            (void)local_chunk_store.store(
                storage::FileChunk{
                    0,
                    recovered_data,
                    chunk_hash
                }
            );

            const std::vector<std::uint8_t>
                published_data =
                    local_chunk_store.load(
                        chunk_hash
                    );

            if (
                published_data !=
                recovered_data
            )
            {
                throw std::runtime_error(
                    "Recovered local chunk does not "
                    "match the verified remote bytes."
                );
            }

            metrics_registry_->
                record_local_chunk_repair();

            logger_->log(
                observability::LogLevel::info,
                "chunk_recovery_succeeded",
                "A missing local chunk was recovered "
                "from a peer.",
                {
                    observability::LogField{
                        "chunk_hash",
                        chunk_hash
                    },
                    observability::LogField{
                        "source_peer_node_id",
                        peer.node_id
                    },
                    observability::LogField{
                        "bytes_recovered",
                        static_cast<std::uint64_t>(
                            recovered_data.size()
                        )
                    },
                    observability::LogField{
                        "peer_attempts",
                        static_cast<std::uint64_t>(
                            report.peer_attempts
                        )
                    }
                }
            );

            report.recovered =
                true;

            report.source_peer_node_id =
                peer.node_id;

            return report;
        }
        catch (const std::exception& error)
        {
            metrics_registry_->
                record_remote_chunk_read(
                    false
                );

            report.failures.push_back(
                ReplicationFailure{
                    peer.node_id,
                    error.what()
                }
            );
        }
    }

    logger_->log(
        observability::LogLevel::error,
        "chunk_recovery_failed",
        "A missing local chunk could not be "
        "recovered from any configured peer.",
        {
            observability::LogField{
                "chunk_hash",
                chunk_hash
            },
            observability::LogField{
                "peer_attempts",
                static_cast<std::uint64_t>(
                    report.peer_attempts
                )
            },
            observability::LogField{
                "peer_failures",
                static_cast<std::uint64_t>(
                    report.failures.size()
                )
            }
        }
    );

    return report;
}

}
