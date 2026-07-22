#include "nexusfs/observability/metrics_registry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nexusfs::observability
{

namespace
{

using Counter =
    std::atomic<std::uint64_t>;

void increment_counter(
    Counter& counter
) noexcept
{
    counter.fetch_add(
        1,
        std::memory_order_relaxed
    );
}

void add_to_counter(
    Counter& counter,
    std::uint64_t value
) noexcept
{
    counter.fetch_add(
        value,
        std::memory_order_relaxed
    );
}

void decrement_without_underflow(
    Counter& counter
) noexcept
{
    std::uint64_t current =
        counter.load(
            std::memory_order_relaxed
        );

    while (current != 0)
    {
        if (
            counter.compare_exchange_weak(
                current,
                current - 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed
            )
        )
        {
            return;
        }
    }
}

void update_maximum(
    Counter& counter,
    std::uint64_t candidate
) noexcept
{
    std::uint64_t current =
        counter.load(
            std::memory_order_relaxed
        );

    while (
        current < candidate
        && !counter.compare_exchange_weak(
            current,
            candidate,
            std::memory_order_relaxed,
            std::memory_order_relaxed
        )
    )
    {
    }
}

std::uint64_t non_negative_microseconds(
    std::chrono::microseconds duration
) noexcept
{
    if (duration.count() <= 0)
    {
        return 0;
    }

    using DurationRepresentation =
        std::chrono::microseconds::rep;

    if constexpr (
        sizeof(DurationRepresentation)
        > sizeof(std::uint64_t)
    )
    {
        const auto maximum =
            static_cast<DurationRepresentation>(
                std::numeric_limits<
                    std::uint64_t
                >::max()
            );

        if (duration.count() > maximum)
        {
            return std::numeric_limits<
                std::uint64_t
            >::max();
        }
    }

    return static_cast<std::uint64_t>(
        duration.count()
    );
}

struct RouteKey
{
    std::string method;
    std::string route;

    [[nodiscard]] bool operator==(
        const RouteKey& other
    ) const noexcept
    {
        return (
            method == other.method
            && route == other.route
        );
    }
};

struct RouteKeyHash
{
    [[nodiscard]] std::size_t operator()(
        const RouteKey& key
    ) const noexcept
    {
        const std::size_t method_hash =
            std::hash<std::string>{}(
                key.method
            );

        const std::size_t route_hash =
            std::hash<std::string>{}(
                key.route
            );

        /*
         * Standard hash-combine construction.
         */
        return (
            method_hash
            ^ (
                route_hash
                + static_cast<std::size_t>(
                    0x9e3779b9U
                )
                + (method_hash << 6U)
                + (method_hash >> 2U)
            )
        );
    }
};

}

struct MetricsRegistry::State
{
    using Clock =
        std::chrono::steady_clock;

    State()
        : started_at{
              Clock::now()
          }
    {
    }

    Clock::time_point started_at;

    Counter connections_total{0};
    Counter connections_active{0};
    Counter connections_peak{0};

    Counter requests_total{0};
    Counter requests_in_flight{0};
    Counter requests_succeeded{0};
    Counter requests_failed{0};

    Counter request_latency_microseconds_total{0};
    Counter request_latency_microseconds_max{0};

    Counter uploads_total{0};
    Counter uploads_succeeded{0};
    Counter uploads_failed{0};
    Counter upload_bytes_processed{0};

    Counter restorations_total{0};
    Counter restorations_succeeded{0};
    Counter restorations_failed{0};
    Counter restoration_bytes_written{0};

    Counter verifications_total{0};
    Counter verifications_succeeded{0};
    Counter verifications_failed{0};
    Counter verification_bytes_processed{0};

    Counter storage_manifests{0};
    Counter storage_complete_manifests{0};
    Counter storage_incomplete_manifests{0};
    Counter storage_chunks{0};
    Counter storage_missing_chunks{0};

    Counter recovery_runs_total{0};
    Counter recovery_entries_scanned{0};
    Counter recovery_temporary_entries_found{0};
    Counter recovery_temporary_files_removed{0};
    Counter recovery_non_regular_entries_preserved{0};

    Counter heartbeat_attempts_total{0};
    Counter heartbeat_attempts_succeeded{0};
    Counter heartbeat_attempts_failed{0};

    Counter replication_chunks_total{0};
    Counter replication_chunks_satisfied{0};
    Counter replication_chunks_failed{0};
    Counter replication_remote_acknowledgements{0};
    Counter replication_remote_failures{0};

    Counter remote_chunk_reads_total{0};
    Counter remote_chunk_reads_succeeded{0};
    Counter remote_chunk_reads_failed{0};
    Counter local_chunk_repairs_total{0};

    Counter replica_maintenance_runs_total{0};
    Counter replica_maintenance_chunks_scanned{0};
    Counter replica_maintenance_local_chunks_recovered{0};
    Counter replica_maintenance_remote_replicas_observed{0};
    Counter replica_maintenance_remote_replicas_created{0};
    Counter replica_maintenance_peer_failures{0};
    Counter replica_maintenance_under_replicated_chunks{0};

    Counter replica_maintenance_scheduler_starts_total{0};
    Counter replica_maintenance_scheduler_stops_total{0};
    Counter replica_maintenance_scheduler_failures_total{0};

    mutable std::mutex dimensions_mutex;

    std::unordered_map<
        std::string,
        std::uint64_t
    > requests_by_method;

    std::unordered_map<
        RouteKey,
        std::uint64_t,
        RouteKeyHash
    > requests_by_route;

    std::unordered_map<
        unsigned int,
        std::uint64_t
    > responses_by_status;
};

MetricsRegistry::MetricsRegistry()
    : state_{
          std::make_unique<State>()
      }
{
}

MetricsRegistry::~MetricsRegistry() = default;

void MetricsRegistry::connection_opened() noexcept
{
    increment_counter(
        state_->connections_total
    );

    const std::uint64_t active_connections =
        state_->connections_active.fetch_add(
            1,
            std::memory_order_relaxed
        )
        + 1;

    update_maximum(
        state_->connections_peak,
        active_connections
    );
}

void MetricsRegistry::connection_closed() noexcept
{
    decrement_without_underflow(
        state_->connections_active
    );
}

void MetricsRegistry::request_started() noexcept
{
    increment_counter(
        state_->requests_total
    );

    increment_counter(
        state_->requests_in_flight
    );
}

void MetricsRegistry::request_finished(
    std::string_view method,
    std::string_view normalized_route,
    unsigned int status_code,
    std::chrono::microseconds latency
) noexcept
{
    const std::uint64_t latency_microseconds =
        non_negative_microseconds(
            latency
        );

    add_to_counter(
        state_->
            request_latency_microseconds_total,
        latency_microseconds
    );

    update_maximum(
        state_->
            request_latency_microseconds_max,
        latency_microseconds
    );

    if (
        status_code >= 200U
        && status_code < 400U
    )
    {
        increment_counter(
            state_->requests_succeeded
        );
    }
    else
    {
        increment_counter(
            state_->requests_failed
        );
    }

    /*
     * Dimensioned counters may allocate. Metrics must never make
     * an otherwise valid request fail, so allocation and container
     * exceptions are intentionally contained here.
     */
    try
    {
        const std::lock_guard lock{
            state_->dimensions_mutex
        };

        ++state_->requests_by_method[
            std::string{
                method
            }
        ];

        ++state_->requests_by_route[
            RouteKey{
                std::string{
                    method
                },
                std::string{
                    normalized_route
                }
            }
        ];

        ++state_->responses_by_status[
            status_code
        ];
    }
    catch (...)
    {
        /*
         * Aggregate counters above remain valid even when a
         * dimensioned metric cannot be recorded.
         */
    }

    decrement_without_underflow(
        state_->requests_in_flight
    );
}

void MetricsRegistry::upload_finished(
    std::uint64_t bytes_processed,
    bool succeeded
) noexcept
{
    increment_counter(
        state_->uploads_total
    );

    if (succeeded)
    {
        increment_counter(
            state_->uploads_succeeded
        );

        add_to_counter(
            state_->upload_bytes_processed,
            bytes_processed
        );
    }
    else
    {
        increment_counter(
            state_->uploads_failed
        );
    }
}

void MetricsRegistry::restoration_finished(
    std::uint64_t bytes_written,
    bool succeeded
) noexcept
{
    increment_counter(
        state_->restorations_total
    );

    if (succeeded)
    {
        increment_counter(
            state_->restorations_succeeded
        );

        add_to_counter(
            state_->restoration_bytes_written,
            bytes_written
        );
    }
    else
    {
        increment_counter(
            state_->restorations_failed
        );
    }
}

void MetricsRegistry::verification_finished(
    std::uint64_t bytes_processed,
    bool succeeded
) noexcept
{
    increment_counter(
        state_->verifications_total
    );

    if (succeeded)
    {
        increment_counter(
            state_->verifications_succeeded
        );

        add_to_counter(
            state_->verification_bytes_processed,
            bytes_processed
        );
    }
    else
    {
        increment_counter(
            state_->verifications_failed
        );
    }
}

void MetricsRegistry::set_storage_catalog(
    std::uint64_t manifests,
    std::uint64_t complete_manifests,
    std::uint64_t incomplete_manifests,
    std::uint64_t chunks,
    std::uint64_t missing_chunks
) noexcept
{
    state_->storage_manifests.store(
        manifests,
        std::memory_order_relaxed
    );

    state_->storage_complete_manifests.store(
        complete_manifests,
        std::memory_order_relaxed
    );

    state_->storage_incomplete_manifests.store(
        incomplete_manifests,
        std::memory_order_relaxed
    );

    state_->storage_chunks.store(
        chunks,
        std::memory_order_relaxed
    );

    state_->storage_missing_chunks.store(
        missing_chunks,
        std::memory_order_relaxed
    );
}

void MetricsRegistry::record_storage_recovery(
    std::uint64_t entries_scanned,
    std::uint64_t temporary_entries_found,
    std::uint64_t temporary_files_removed,
    std::uint64_t non_regular_entries_preserved
) noexcept
{
    increment_counter(
        state_->recovery_runs_total
    );

    add_to_counter(
        state_->recovery_entries_scanned,
        entries_scanned
    );

    add_to_counter(
        state_->
            recovery_temporary_entries_found,
        temporary_entries_found
    );

    add_to_counter(
        state_->
            recovery_temporary_files_removed,
        temporary_files_removed
    );

    add_to_counter(
        state_->
            recovery_non_regular_entries_preserved,
        non_regular_entries_preserved
    );
}

void MetricsRegistry::record_heartbeat_attempt(
    bool succeeded
) noexcept
{
    increment_counter(
        state_->heartbeat_attempts_total
    );

    if (succeeded)
    {
        increment_counter(
            state_->
                heartbeat_attempts_succeeded
        );
    }
    else
    {
        increment_counter(
            state_->heartbeat_attempts_failed
        );
    }
}

void MetricsRegistry::record_replication_result(
    std::uint64_t remote_acknowledgements,
    std::uint64_t remote_failures,
    bool satisfied
) noexcept
{
    increment_counter(
        state_->replication_chunks_total
    );

    add_to_counter(
        state_->
            replication_remote_acknowledgements,
        remote_acknowledgements
    );

    add_to_counter(
        state_->
            replication_remote_failures,
        remote_failures
    );

    if (satisfied)
    {
        increment_counter(
            state_->
                replication_chunks_satisfied
        );
    }
    else
    {
        increment_counter(
            state_->replication_chunks_failed
        );
    }
}

void MetricsRegistry::record_remote_chunk_read(
    bool succeeded
) noexcept
{
    increment_counter(
        state_->remote_chunk_reads_total
    );

    if (succeeded)
    {
        increment_counter(
            state_->
                remote_chunk_reads_succeeded
        );
    }
    else
    {
        increment_counter(
            state_->remote_chunk_reads_failed
        );
    }
}

void MetricsRegistry::record_local_chunk_repair()
    noexcept
{
    increment_counter(
        state_->local_chunk_repairs_total
    );
}

void MetricsRegistry::record_replica_maintenance(
    std::uint64_t chunks_scanned,
    std::uint64_t local_chunks_recovered,
    std::uint64_t remote_replicas_observed,
    std::uint64_t remote_replicas_created,
    std::uint64_t peer_failures,
    std::uint64_t under_replicated_chunks
) noexcept
{
    increment_counter(
        state_->
            replica_maintenance_runs_total
    );

    add_to_counter(
        state_->
            replica_maintenance_chunks_scanned,
        chunks_scanned
    );

    add_to_counter(
        state_->
            replica_maintenance_local_chunks_recovered,
        local_chunks_recovered
    );

    add_to_counter(
        state_->
            replica_maintenance_remote_replicas_observed,
        remote_replicas_observed
    );

    add_to_counter(
        state_->
            replica_maintenance_remote_replicas_created,
        remote_replicas_created
    );

    add_to_counter(
        state_->
            replica_maintenance_peer_failures,
        peer_failures
    );

    add_to_counter(
        state_->
            replica_maintenance_under_replicated_chunks,
        under_replicated_chunks
    );
}

void MetricsRegistry::
record_replica_maintenance_scheduler_started()
    noexcept
{
    increment_counter(
        state_->
            replica_maintenance_scheduler_starts_total
    );
}

void MetricsRegistry::
record_replica_maintenance_scheduler_stopped()
    noexcept
{
    increment_counter(
        state_->
            replica_maintenance_scheduler_stops_total
    );
}

void MetricsRegistry::
record_replica_maintenance_scheduler_failure()
    noexcept
{
    increment_counter(
        state_->
            replica_maintenance_scheduler_failures_total
    );
}

MetricsSnapshot MetricsRegistry::snapshot() const
{
    MetricsSnapshot result;

    const State::Clock::time_point now =
        State::Clock::now();

    const auto uptime =
        std::chrono::duration_cast<
            std::chrono::milliseconds
        >(
            now
            - state_->started_at
        );

    result.uptime_milliseconds =
        uptime.count() > 0
        ? static_cast<std::uint64_t>(
              uptime.count()
          )
        : 0;

    result.connections_total =
        state_->connections_total.load(
            std::memory_order_relaxed
        );

    result.connections_active =
        state_->connections_active.load(
            std::memory_order_relaxed
        );

    result.connections_peak =
        state_->connections_peak.load(
            std::memory_order_relaxed
        );

    result.requests_total =
        state_->requests_total.load(
            std::memory_order_relaxed
        );

    result.requests_in_flight =
        state_->requests_in_flight.load(
            std::memory_order_relaxed
        );

    result.requests_succeeded =
        state_->requests_succeeded.load(
            std::memory_order_relaxed
        );

    result.requests_failed =
        state_->requests_failed.load(
            std::memory_order_relaxed
        );

    result.request_latency_microseconds_total =
        state_->
            request_latency_microseconds_total.load(
                std::memory_order_relaxed
            );

    result.request_latency_microseconds_max =
        state_->
            request_latency_microseconds_max.load(
                std::memory_order_relaxed
            );

    result.uploads_total =
        state_->uploads_total.load(
            std::memory_order_relaxed
        );

    result.uploads_succeeded =
        state_->uploads_succeeded.load(
            std::memory_order_relaxed
        );

    result.uploads_failed =
        state_->uploads_failed.load(
            std::memory_order_relaxed
        );

    result.upload_bytes_processed =
        state_->upload_bytes_processed.load(
            std::memory_order_relaxed
        );

    result.restorations_total =
        state_->restorations_total.load(
            std::memory_order_relaxed
        );

    result.restorations_succeeded =
        state_->restorations_succeeded.load(
            std::memory_order_relaxed
        );

    result.restorations_failed =
        state_->restorations_failed.load(
            std::memory_order_relaxed
        );

    result.restoration_bytes_written =
        state_->restoration_bytes_written.load(
            std::memory_order_relaxed
        );

    result.verifications_total =
        state_->verifications_total.load(
            std::memory_order_relaxed
        );

    result.verifications_succeeded =
        state_->verifications_succeeded.load(
            std::memory_order_relaxed
        );

    result.verifications_failed =
        state_->verifications_failed.load(
            std::memory_order_relaxed
        );

    result.verification_bytes_processed =
        state_->
            verification_bytes_processed.load(
                std::memory_order_relaxed
            );

    result.storage_manifests =
        state_->storage_manifests.load(
            std::memory_order_relaxed
        );

    result.storage_complete_manifests =
        state_->
            storage_complete_manifests.load(
                std::memory_order_relaxed
            );

    result.storage_incomplete_manifests =
        state_->
            storage_incomplete_manifests.load(
                std::memory_order_relaxed
            );

    result.storage_chunks =
        state_->storage_chunks.load(
            std::memory_order_relaxed
        );

    result.storage_missing_chunks =
        state_->storage_missing_chunks.load(
            std::memory_order_relaxed
        );

    result.recovery_runs_total =
        state_->recovery_runs_total.load(
            std::memory_order_relaxed
        );

    result.recovery_entries_scanned =
        state_->recovery_entries_scanned.load(
            std::memory_order_relaxed
        );

    result.recovery_temporary_entries_found =
        state_->
            recovery_temporary_entries_found.load(
                std::memory_order_relaxed
            );

    result.recovery_temporary_files_removed =
        state_->
            recovery_temporary_files_removed.load(
                std::memory_order_relaxed
            );

    result.recovery_non_regular_entries_preserved =
        state_->
            recovery_non_regular_entries_preserved.load(
                std::memory_order_relaxed
            );

    result.heartbeat_attempts_total =
        state_->heartbeat_attempts_total.load(
            std::memory_order_relaxed
        );

    result.heartbeat_attempts_succeeded =
        state_->
            heartbeat_attempts_succeeded.load(
                std::memory_order_relaxed
            );

    result.heartbeat_attempts_failed =
        state_->heartbeat_attempts_failed.load(
            std::memory_order_relaxed
        );

    result.replication_chunks_total =
        state_->replication_chunks_total.load(
            std::memory_order_relaxed
        );

    result.replication_chunks_satisfied =
        state_->
            replication_chunks_satisfied.load(
                std::memory_order_relaxed
            );

    result.replication_chunks_failed =
        state_->replication_chunks_failed.load(
            std::memory_order_relaxed
        );

    result.replication_remote_acknowledgements =
        state_->
            replication_remote_acknowledgements.load(
                std::memory_order_relaxed
            );

    result.replication_remote_failures =
        state_->
            replication_remote_failures.load(
                std::memory_order_relaxed
            );

    result.remote_chunk_reads_total =
        state_->remote_chunk_reads_total.load(
            std::memory_order_relaxed
        );

    result.remote_chunk_reads_succeeded =
        state_->
            remote_chunk_reads_succeeded.load(
                std::memory_order_relaxed
            );

    result.remote_chunk_reads_failed =
        state_->remote_chunk_reads_failed.load(
            std::memory_order_relaxed
        );

    result.local_chunk_repairs_total =
        state_->local_chunk_repairs_total.load(
            std::memory_order_relaxed
        );

    result.replica_maintenance_runs_total =
        state_->
            replica_maintenance_runs_total.load(
                std::memory_order_relaxed
            );

    result.replica_maintenance_chunks_scanned =
        state_->
            replica_maintenance_chunks_scanned.load(
                std::memory_order_relaxed
            );

    result.replica_maintenance_local_chunks_recovered =
        state_->
            replica_maintenance_local_chunks_recovered.load(
                std::memory_order_relaxed
            );

    result.replica_maintenance_remote_replicas_observed =
        state_->
            replica_maintenance_remote_replicas_observed.load(
                std::memory_order_relaxed
            );

    result.replica_maintenance_remote_replicas_created =
        state_->
            replica_maintenance_remote_replicas_created.load(
                std::memory_order_relaxed
            );

    result.replica_maintenance_peer_failures =
        state_->
            replica_maintenance_peer_failures.load(
                std::memory_order_relaxed
            );

    result.replica_maintenance_under_replicated_chunks =
        state_->
            replica_maintenance_under_replicated_chunks.load(
                std::memory_order_relaxed
            );

    result.replica_maintenance_scheduler_starts_total =
        state_->
            replica_maintenance_scheduler_starts_total.load(
                std::memory_order_relaxed
            );

    result.replica_maintenance_scheduler_stops_total =
        state_->
            replica_maintenance_scheduler_stops_total.load(
                std::memory_order_relaxed
            );

    result.replica_maintenance_scheduler_failures_total =
        state_->
            replica_maintenance_scheduler_failures_total.load(
                std::memory_order_relaxed
            );

    {
        const std::lock_guard lock{
            state_->dimensions_mutex
        };

        result.requests_by_method.reserve(
            state_->
                requests_by_method.size()
        );

        for (
            const auto& [
                method,
                count
            ] :
            state_->requests_by_method
        )
        {
            result.requests_by_method.push_back(
                NamedCounter{
                    method,
                    count
                }
            );
        }

        result.requests_by_route.reserve(
            state_->
                requests_by_route.size()
        );

        for (
            const auto& [
                route_key,
                count
            ] :
            state_->requests_by_route
        )
        {
            result.requests_by_route.push_back(
                HttpRouteCounter{
                    route_key.method,
                    route_key.route,
                    count
                }
            );
        }

        result.responses_by_status.reserve(
            state_->
                responses_by_status.size()
        );

        for (
            const auto& [
                status_code,
                count
            ] :
            state_->responses_by_status
        )
        {
            result.responses_by_status.push_back(
                HttpStatusCounter{
                    status_code,
                    count
                }
            );
        }
    }

    std::sort(
        result.requests_by_method.begin(),
        result.requests_by_method.end(),
        [](
            const NamedCounter& left,
            const NamedCounter& right
        )
        {
            return left.name < right.name;
        }
    );

    std::sort(
        result.requests_by_route.begin(),
        result.requests_by_route.end(),
        [](
            const HttpRouteCounter& left,
            const HttpRouteCounter& right
        )
        {
            if (left.route != right.route)
            {
                return left.route < right.route;
            }

            return left.method < right.method;
        }
    );

    std::sort(
        result.responses_by_status.begin(),
        result.responses_by_status.end(),
        [](
            const HttpStatusCounter& left,
            const HttpStatusCounter& right
        )
        {
            return (
                left.status_code
                < right.status_code
            );
        }
    );

    return result;
}

}