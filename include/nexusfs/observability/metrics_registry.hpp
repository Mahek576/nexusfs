#ifndef NEXUSFS_OBSERVABILITY_METRICS_REGISTRY_HPP
#define NEXUSFS_OBSERVABILITY_METRICS_REGISTRY_HPP

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nexusfs::observability
{

struct NamedCounter
{
    std::string name;
    std::uint64_t count{0};
};

struct HttpRouteCounter
{
    std::string method;
    std::string route;
    std::uint64_t count{0};
};

struct HttpStatusCounter
{
    unsigned int status_code{0};
    std::uint64_t count{0};
};

struct MetricsSnapshot
{
    std::uint64_t uptime_milliseconds{0};

    std::uint64_t connections_total{0};
    std::uint64_t connections_active{0};
    std::uint64_t connections_peak{0};

    std::uint64_t requests_total{0};
    std::uint64_t requests_in_flight{0};
    std::uint64_t requests_succeeded{0};
    std::uint64_t requests_failed{0};

    std::uint64_t request_latency_microseconds_total{0};
    std::uint64_t request_latency_microseconds_max{0};

    std::uint64_t uploads_total{0};
    std::uint64_t uploads_succeeded{0};
    std::uint64_t uploads_failed{0};
    std::uint64_t upload_bytes_processed{0};

    std::uint64_t restorations_total{0};
    std::uint64_t restorations_succeeded{0};
    std::uint64_t restorations_failed{0};
    std::uint64_t restoration_bytes_written{0};

    std::uint64_t verifications_total{0};
    std::uint64_t verifications_succeeded{0};
    std::uint64_t verifications_failed{0};
    std::uint64_t verification_bytes_processed{0};

    std::uint64_t storage_manifests{0};
    std::uint64_t storage_complete_manifests{0};
    std::uint64_t storage_incomplete_manifests{0};
    std::uint64_t storage_chunks{0};
    std::uint64_t storage_missing_chunks{0};

    std::uint64_t recovery_runs_total{0};
    std::uint64_t recovery_entries_scanned{0};
    std::uint64_t recovery_temporary_entries_found{0};
    std::uint64_t recovery_temporary_files_removed{0};
    std::uint64_t recovery_non_regular_entries_preserved{0};

    std::uint64_t heartbeat_attempts_total{0};
    std::uint64_t heartbeat_attempts_succeeded{0};
    std::uint64_t heartbeat_attempts_failed{0};

    std::uint64_t replication_chunks_total{0};
    std::uint64_t replication_chunks_satisfied{0};
    std::uint64_t replication_chunks_failed{0};
    std::uint64_t replication_remote_acknowledgements{0};
    std::uint64_t replication_remote_failures{0};

    std::uint64_t remote_chunk_reads_total{0};
    std::uint64_t remote_chunk_reads_succeeded{0};
    std::uint64_t remote_chunk_reads_failed{0};
    std::uint64_t local_chunk_repairs_total{0};

    std::vector<NamedCounter> requests_by_method;
    std::vector<HttpRouteCounter> requests_by_route;
    std::vector<HttpStatusCounter> responses_by_status;
};

/*
 * Thread-safe process-local operational metrics.
 *
 * Hot aggregate counters use atomics. Dimensioned counters such as
 * HTTP methods, normalized routes and status codes use an internal
 * mutex because they require dynamically sized maps.
 *
 * Metrics collection is deliberately best-effort: instrumentation
 * methods are noexcept and never allow allocation failures inside
 * the metrics layer to break a NexusFS request.
 */
class MetricsRegistry final
{
public:
    MetricsRegistry();

    ~MetricsRegistry();

    MetricsRegistry(
        const MetricsRegistry&
    ) = delete;

    MetricsRegistry& operator=(
        const MetricsRegistry&
    ) = delete;

    MetricsRegistry(
        MetricsRegistry&&
    ) = delete;

    MetricsRegistry& operator=(
        MetricsRegistry&&
    ) = delete;

    /*
     * Connection lifecycle.
     *
     * One call to connection_opened() must be paired with one call
     * to connection_closed().
     */
    void connection_opened() noexcept;

    void connection_closed() noexcept;

    /*
     * HTTP request lifecycle.
     *
     * request_started() increments total and in-flight requests.
     * request_finished() records the final dimensions, latency and
     * outcome, then decrements the in-flight gauge.
     */
    void request_started() noexcept;

    void request_finished(
        std::string_view method,
        std::string_view normalized_route,
        unsigned int status_code,
        std::chrono::microseconds latency
    ) noexcept;

    /*
     * Storage-operation counters.
     *
     * Byte counters include bytes from successful operations only.
     */
    void upload_finished(
        std::uint64_t bytes_processed,
        bool succeeded
    ) noexcept;

    void restoration_finished(
        std::uint64_t bytes_written,
        bool succeeded
    ) noexcept;

    void verification_finished(
        std::uint64_t bytes_processed,
        bool succeeded
    ) noexcept;

    /*
     * Replaces storage catalog gauges with the latest observed
     * values. These are gauges rather than monotonically increasing
     * counters because future garbage collection and deletion can
     * reduce them.
     */
    void set_storage_catalog(
        std::uint64_t manifests,
        std::uint64_t complete_manifests,
        std::uint64_t incomplete_manifests,
        std::uint64_t chunks,
        std::uint64_t missing_chunks
    ) noexcept;

    /*
     * Records one completed startup-recovery scan.
     */
    void record_storage_recovery(
        std::uint64_t entries_scanned,
        std::uint64_t temporary_entries_found,
        std::uint64_t temporary_files_removed,
        std::uint64_t non_regular_entries_preserved
    ) noexcept;

    /*
     * Records one outbound peer-heartbeat attempt.
     */
    void record_heartbeat_attempt(
        bool succeeded
    ) noexcept;

    /*
     * Records the result of replicating one chunk.
     */
    void record_replication_result(
        std::uint64_t remote_acknowledgements,
        std::uint64_t remote_failures,
        bool satisfied
    ) noexcept;

    /*
     * Records one remote chunk-read attempt.
     */
    void record_remote_chunk_read(
        bool succeeded
    ) noexcept;

    /*
     * Records one verified local chunk repair.
     */
    void record_local_chunk_repair() noexcept;

    /*
     * Produces a self-consistent best-effort metrics snapshot.
     *
     * The snapshot can allocate while copying dimensioned counters,
     * so this method is intentionally not noexcept.
     */
    [[nodiscard]] MetricsSnapshot snapshot() const;

private:
    struct State;

    std::unique_ptr<State> state_;
};

}

#endif