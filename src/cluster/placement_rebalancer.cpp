#include "nexusfs/cluster/placement_rebalancer.hpp"

#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nexusfs::cluster
{

namespace
{

bool is_lowercase_sha256(
    std::string_view value
) noexcept
{
    return (
        value.size() == 64
        && std::all_of(
            value.begin(),
            value.end(),
            [](char character)
            {
                return (
                    (
                        character >= '0'
                        && character <= '9'
                    )
                    || (
                        character >= 'a'
                        && character <= 'f'
                    )
                );
            }
        )
    );
}

std::vector<std::string>
canonical_chunk_hashes(
    const std::vector<std::string>& chunk_hashes
)
{
    std::vector<std::string> canonical =
        chunk_hashes;

    for (
        const std::string& chunk_hash :
        canonical
    )
    {
        if (
            !is_lowercase_sha256(
                chunk_hash
            )
        )
        {
            throw std::invalid_argument(
                "Rebalance chunk hash must contain "
                "64 lowercase hexadecimal characters."
            );
        }
    }

    std::sort(
        canonical.begin(),
        canonical.end()
    );

    canonical.erase(
        std::unique(
            canonical.begin(),
            canonical.end()
        ),
        canonical.end()
    );

    return canonical;
}

std::string calculate_request_digest(
    std::string_view operation_id,
    std::uint64_t membership_epoch,
    std::size_t replication_factor,
    const std::vector<std::string>& chunk_hashes
)
{
    std::string canonical{
        "nexusfs-rebalance-v1\n"
    };

    canonical +=
        std::string{
            operation_id
        };

    canonical += "\n";

    canonical +=
        std::to_string(
            membership_epoch
        );

    canonical += "\n";

    canonical +=
        std::to_string(
            replication_factor
        );

    canonical += "\n";

    for (
        const std::string& chunk_hash :
        chunk_hashes
    )
    {
        canonical += chunk_hash;
        canonical += "\n";
    }

    const std::vector<std::uint8_t> bytes{
        canonical.begin(),
        canonical.end()
    };

    return storage::Sha256Hasher::hash(
        std::span<const std::uint8_t>{
            bytes.data(),
            bytes.size()
        }
    );
}

PlacementRebalanceReport report_from_record(
    const RebalanceOperationRecord& record
)
{
    return PlacementRebalanceReport{
        RebalanceStatus::replayed,
        record.operation_id,
        record.request_digest,
        record.membership_epoch,
        record.membership_epoch,
        record.replication_factor,
        record.chunks_scanned,
        record.targets_planned,
        record.replicas_observed,
        record.replicas_created,
        record.peer_failures,
        record.under_replicated_chunks,
        record.converged,
        true,
        false
    };
}

RebalanceOperationRecord record_from_report(
    const PlacementRebalanceReport& report
)
{
    return RebalanceOperationRecord{
        report.operation_id,
        report.request_digest,
        report.expected_membership_epoch,
        report.replication_factor,
        report.chunks_scanned,
        report.targets_planned,
        report.replicas_observed,
        report.replicas_created,
        report.peer_failures,
        report.under_replicated_chunks,
        report.converged
    };
}

}

std::string_view rebalance_status_name(
    RebalanceStatus status
) noexcept
{
    switch (status)
    {
        case RebalanceStatus::completed:
            return "completed";

        case RebalanceStatus::replayed:
            return "replayed";

        case RebalanceStatus::
            stale_membership_epoch:
            return "stale_membership_epoch";
    }

    return "stale_membership_epoch";
}

PlacementRebalancer::PlacementRebalancer(
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
      journal_{
          cluster_node_
              ? cluster_node_
                    ->cluster_directory()
              : std::filesystem::path{}
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
            "Placement rebalancer cluster node "
            "cannot be null."
        );
    }

    if (replication_factor_ == 0)
    {
        throw std::invalid_argument(
            "Placement rebalancer replication factor "
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

PlacementRebalanceReport
PlacementRebalancer::rebalance(
    const std::vector<std::string>& chunk_hashes,
    storage::ChunkStore& local_chunk_store,
    std::string operation_id,
    std::uint64_t expected_membership_epoch
)
{
    const std::vector<std::string>
        canonical_hashes =
            canonical_chunk_hashes(
                chunk_hashes
            );

    const std::string request_digest =
        calculate_request_digest(
            operation_id,
            expected_membership_epoch,
            replication_factor_,
            canonical_hashes
        );

    if (
        const auto existing =
            journal_.load_rebalance(
                operation_id
            )
    )
    {
        if (
            existing->request_digest !=
            request_digest
        )
        {
            metrics_registry_->
                record_rebalance_idempotency_conflict();

            logger_->log(
                observability::LogLevel::error,
                "cluster_rebalance_idempotency_conflict",
                "A rebalance operation ID was reused "
                "with a different request.",
                {
                    observability::LogField{
                        "operation_id",
                        operation_id
                    },
                    observability::LogField{
                        "existing_request_digest",
                        existing->request_digest
                    },
                    observability::LogField{
                        "supplied_request_digest",
                        request_digest
                    }
                }
            );

            throw OperationIdConflict(
                "Rebalance operation ID has already "
                "been used for a different request."
            );
        }

        PlacementRebalanceReport report =
            report_from_record(
                *existing
            );

        metrics_registry_->
            record_rebalance_result(
                report.chunks_scanned,
                report.targets_planned,
                report.replicas_observed,
                report.replicas_created,
                report.peer_failures,
                report.under_replicated_chunks,
                true,
                true,
                false
            );

        logger_->log(
            observability::LogLevel::info,
            "cluster_rebalance_replayed",
            "A completed rebalance result was "
            "replayed from the durable journal.",
            {
                observability::LogField{
                    "operation_id",
                    operation_id
                },
                observability::LogField{
                    "request_digest",
                    request_digest
                },
                observability::LogField{
                    "membership_epoch",
                    report.expected_membership_epoch
                },
                observability::LogField{
                    "converged",
                    report.converged
                }
            }
        );

        return report;
    }

    PlacementRebalanceReport report{
        RebalanceStatus::completed,
        std::move(operation_id),
        request_digest,
        expected_membership_epoch,
        cluster_node_->membership_epoch(),
        static_cast<std::uint64_t>(
            replication_factor_
        )
    };

    const auto finish_stale =
        [&]()
        {
            report.status =
                RebalanceStatus::
                    stale_membership_epoch;

            report.observed_membership_epoch =
                cluster_node_->membership_epoch();

            report.converged =
                false;

            report.replayed =
                false;

            report.applied =
                report.replicas_created != 0;

            metrics_registry_->
                record_rebalance_result(
                    report.chunks_scanned,
                    report.targets_planned,
                    report.replicas_observed,
                    report.replicas_created,
                    report.peer_failures,
                    report.under_replicated_chunks,
                    false,
                    false,
                    true
                );

            logger_->log(
                observability::LogLevel::warning,
                "cluster_rebalance_stale_epoch",
                "Rebalancing was fenced because the "
                "membership epoch changed.",
                {
                    observability::LogField{
                        "operation_id",
                        report.operation_id
                    },
                    observability::LogField{
                        "expected_membership_epoch",
                        report.expected_membership_epoch
                    },
                    observability::LogField{
                        "observed_membership_epoch",
                        report.observed_membership_epoch
                    },
                    observability::LogField{
                        "replicas_created_before_fence",
                        report.replicas_created
                    }
                }
            );

            return report;
        };

    if (
        report.observed_membership_epoch !=
        expected_membership_epoch
    )
    {
        return finish_stale();
    }

    const std::vector<PeerDefinition> peers =
        cluster_node_->peer_definitions();

    const std::size_t
        required_remote_replicas =
            replication_factor_ - 1;

    for (
        const std::string& chunk_hash :
        canonical_hashes
    )
    {
        if (
            cluster_node_->membership_epoch() !=
            expected_membership_epoch
        )
        {
            return finish_stale();
        }

        ++report.chunks_scanned;

        /*
         * load() validates the local content-addressed object before
         * it can be used as a rebalance source.
         */
        const std::vector<std::uint8_t> data =
            local_chunk_store.load(
                chunk_hash
            );

        if (required_remote_replicas == 0)
        {
            continue;
        }

        const std::vector<PeerDefinition> targets =
            ReplicationCoordinator::
                select_replica_peers(
                    chunk_hash,
                    peers,
                    required_remote_replicas
                );

        report.targets_planned +=
            static_cast<std::uint64_t>(
                targets.size()
            );

        std::size_t confirmed_replicas =
            0;

        for (
            const PeerDefinition& peer :
            targets
        )
        {
            if (
                cluster_node_->membership_epoch() !=
                expected_membership_epoch
            )
            {
                return finish_stale();
            }

            try
            {
                if (
                    transport_.chunk_exists(
                        peer,
                        chunk_hash
                    )
                )
                {
                    ++report.replicas_observed;
                }
                else
                {
                    const RemoteChunkStoreResult result =
                        transport_.store_chunk(
                            peer,
                            chunk_hash,
                            data
                        );

                    if (
                        result ==
                        RemoteChunkStoreResult::stored
                    )
                    {
                        ++report.replicas_created;
                    }
                    else
                    {
                        ++report.replicas_observed;
                    }
                }

                ++confirmed_replicas;
            }
            catch (const std::exception&)
            {
                ++report.peer_failures;
            }
        }

        if (
            confirmed_replicas <
            required_remote_replicas
        )
        {
            ++report.under_replicated_chunks;
        }
    }

    if (
        cluster_node_->membership_epoch() !=
        expected_membership_epoch
    )
    {
        return finish_stale();
    }

    report.status =
        RebalanceStatus::completed;

    report.observed_membership_epoch =
        expected_membership_epoch;

    report.converged =
        report.under_replicated_chunks == 0;

    report.replayed =
        false;

    report.applied =
        true;

    const RebalanceOperationRecord persisted =
        journal_.publish_rebalance(
            record_from_report(
                report
            )
        );

    if (
        persisted !=
        record_from_report(
            report
        )
    )
    {
        PlacementRebalanceReport replayed =
            report_from_record(
                persisted
            );

        metrics_registry_->
            record_rebalance_result(
                replayed.chunks_scanned,
                replayed.targets_planned,
                replayed.replicas_observed,
                replayed.replicas_created,
                replayed.peer_failures,
                replayed.under_replicated_chunks,
                true,
                true,
                false
            );

        return replayed;
    }

    metrics_registry_->
        record_rebalance_result(
            report.chunks_scanned,
            report.targets_planned,
            report.replicas_observed,
            report.replicas_created,
            report.peer_failures,
            report.under_replicated_chunks,
            true,
            false,
            false
        );

    logger_->log(
        report.converged
            ? observability::LogLevel::info
            : observability::LogLevel::warning,
        "cluster_rebalance_completed",
        report.converged
            ? "Deterministic placement rebalancing converged."
            : "Deterministic placement rebalancing completed "
              "with under-replicated chunks.",
        {
            observability::LogField{
                "operation_id",
                report.operation_id
            },
            observability::LogField{
                "request_digest",
                report.request_digest
            },
            observability::LogField{
                "membership_epoch",
                report.expected_membership_epoch
            },
            observability::LogField{
                "chunks_scanned",
                report.chunks_scanned
            },
            observability::LogField{
                "targets_planned",
                report.targets_planned
            },
            observability::LogField{
                "replicas_observed",
                report.replicas_observed
            },
            observability::LogField{
                "replicas_created",
                report.replicas_created
            },
            observability::LogField{
                "peer_failures",
                report.peer_failures
            },
            observability::LogField{
                "under_replicated_chunks",
                report.under_replicated_chunks
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
