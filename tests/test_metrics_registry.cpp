#include "nexusfs/observability/metrics_registry.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

using nexusfs::observability::HttpRouteCounter;
using nexusfs::observability::HttpStatusCounter;
using nexusfs::observability::MetricsRegistry;
using nexusfs::observability::MetricsSnapshot;
using nexusfs::observability::NamedCounter;

void require_true(
    bool condition,
    const std::string& test_name
)
{
    if (!condition)
    {
        throw std::runtime_error(
            test_name + " failed."
        );
    }
}

template <typename Actual, typename Expected>
void require_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& test_name
)
{
    if (actual != expected)
    {
        throw std::runtime_error(
            test_name + " failed."
        );
    }
}

const NamedCounter& find_named_counter(
    const std::vector<NamedCounter>& counters,
    const std::string& name
)
{
    for (
        const NamedCounter& counter :
        counters
    )
    {
        if (counter.name == name)
        {
            return counter;
        }
    }

    throw std::runtime_error(
        "Named counter was not found: "
        + name
    );
}

const HttpRouteCounter& find_route_counter(
    const std::vector<HttpRouteCounter>& counters,
    const std::string& method,
    const std::string& route
)
{
    for (
        const HttpRouteCounter& counter :
        counters
    )
    {
        if (
            counter.method == method
            && counter.route == route
        )
        {
            return counter;
        }
    }

    throw std::runtime_error(
        "Route counter was not found: "
        + method
        + " "
        + route
    );
}

const HttpStatusCounter& find_status_counter(
    const std::vector<HttpStatusCounter>& counters,
    unsigned int status_code
)
{
    for (
        const HttpStatusCounter& counter :
        counters
    )
    {
        if (counter.status_code == status_code)
        {
            return counter;
        }
    }

    throw std::runtime_error(
        "Status counter was not found: "
        + std::to_string(
            status_code
        )
    );
}

void require_sorted_dimensions(
    const MetricsSnapshot& snapshot
)
{
    for (
        std::size_t index = 1;
        index <
            snapshot.requests_by_method.size();
        ++index
    )
    {
        require_true(
            snapshot.requests_by_method[
                index - 1
            ].name
                <
            snapshot.requests_by_method[
                index
            ].name,
            "Method-counter ordering test"
        );
    }

    for (
        std::size_t index = 1;
        index <
            snapshot.requests_by_route.size();
        ++index
    )
    {
        const HttpRouteCounter& previous =
            snapshot.requests_by_route[
                index - 1
            ];

        const HttpRouteCounter& current =
            snapshot.requests_by_route[
                index
            ];

        const bool correctly_ordered =
            (
                previous.route
                < current.route
            )
            || (
                previous.route
                    == current.route
                && previous.method
                    < current.method
            );

        require_true(
            correctly_ordered,
            "Route-counter ordering test"
        );
    }

    for (
        std::size_t index = 1;
        index <
            snapshot.responses_by_status.size();
        ++index
    )
    {
        require_true(
            snapshot.responses_by_status[
                index - 1
            ].status_code
                <
            snapshot.responses_by_status[
                index
            ].status_code,
            "Status-counter ordering test"
        );
    }
}

void test_initial_snapshot()
{
    MetricsRegistry registry;

    const MetricsSnapshot initial =
        registry.snapshot();

    require_equal(
        initial.connections_total,
        static_cast<std::uint64_t>(0),
        "Initial total-connections test"
    );

    require_equal(
        initial.connections_active,
        static_cast<std::uint64_t>(0),
        "Initial active-connections test"
    );

    require_equal(
        initial.connections_peak,
        static_cast<std::uint64_t>(0),
        "Initial peak-connections test"
    );

    require_equal(
        initial.requests_total,
        static_cast<std::uint64_t>(0),
        "Initial request-count test"
    );

    require_equal(
        initial.requests_in_flight,
        static_cast<std::uint64_t>(0),
        "Initial in-flight request test"
    );

    require_equal(
        initial.requests_succeeded,
        static_cast<std::uint64_t>(0),
        "Initial successful-request test"
    );

    require_equal(
        initial.requests_failed,
        static_cast<std::uint64_t>(0),
        "Initial failed-request test"
    );

    require_true(
        initial.requests_by_method.empty(),
        "Initial method-dimensions test"
    );

    require_true(
        initial.requests_by_route.empty(),
        "Initial route-dimensions test"
    );

    require_true(
        initial.responses_by_status.empty(),
        "Initial status-dimensions test"
    );

    std::this_thread::sleep_for(
        std::chrono::milliseconds{
            2
        }
    );

    const MetricsSnapshot later =
        registry.snapshot();

    require_true(
        later.uptime_milliseconds
            >= initial.uptime_milliseconds,
        "Monotonic uptime test"
    );
}

void test_connection_lifecycle()
{
    MetricsRegistry registry;

    /*
     * An unmatched close must not underflow the active gauge.
     */
    registry.connection_closed();

    registry.connection_opened();
    registry.connection_opened();
    registry.connection_opened();

    registry.connection_closed();

    MetricsSnapshot snapshot =
        registry.snapshot();

    require_equal(
        snapshot.connections_total,
        static_cast<std::uint64_t>(3),
        "Connection total test"
    );

    require_equal(
        snapshot.connections_active,
        static_cast<std::uint64_t>(2),
        "Connection active test"
    );

    require_equal(
        snapshot.connections_peak,
        static_cast<std::uint64_t>(3),
        "Connection peak test"
    );

    registry.connection_closed();
    registry.connection_closed();
    registry.connection_closed();

    snapshot =
        registry.snapshot();

    require_equal(
        snapshot.connections_active,
        static_cast<std::uint64_t>(0),
        "Connection underflow-protection test"
    );

    require_equal(
        snapshot.connections_peak,
        static_cast<std::uint64_t>(3),
        "Connection peak-retention test"
    );
}

void test_request_metrics()
{
    MetricsRegistry registry;

    registry.request_started();

    registry.request_finished(
        "GET",
        "/api/v1/health",
        200,
        std::chrono::microseconds{
            150
        }
    );

    registry.request_started();

    registry.request_finished(
        "POST",
        "/api/v1/files",
        201,
        std::chrono::microseconds{
            250
        }
    );

    registry.request_started();

    registry.request_finished(
        "GET",
        "/unknown",
        404,
        std::chrono::microseconds{
            50
        }
    );

    const MetricsSnapshot snapshot =
        registry.snapshot();

    require_equal(
        snapshot.requests_total,
        static_cast<std::uint64_t>(3),
        "Request total test"
    );

    require_equal(
        snapshot.requests_in_flight,
        static_cast<std::uint64_t>(0),
        "Request in-flight completion test"
    );

    require_equal(
        snapshot.requests_succeeded,
        static_cast<std::uint64_t>(2),
        "Successful request test"
    );

    require_equal(
        snapshot.requests_failed,
        static_cast<std::uint64_t>(1),
        "Failed request test"
    );

    require_equal(
        snapshot
            .request_latency_microseconds_total,
        static_cast<std::uint64_t>(450),
        "Request latency-total test"
    );

    require_equal(
        snapshot
            .request_latency_microseconds_max,
        static_cast<std::uint64_t>(250),
        "Request latency-maximum test"
    );

    require_equal(
        find_named_counter(
            snapshot.requests_by_method,
            "GET"
        ).count,
        static_cast<std::uint64_t>(2),
        "GET method-counter test"
    );

    require_equal(
        find_named_counter(
            snapshot.requests_by_method,
            "POST"
        ).count,
        static_cast<std::uint64_t>(1),
        "POST method-counter test"
    );

    require_equal(
        find_route_counter(
            snapshot.requests_by_route,
            "GET",
            "/api/v1/health"
        ).count,
        static_cast<std::uint64_t>(1),
        "Health route-counter test"
    );

    require_equal(
        find_route_counter(
            snapshot.requests_by_route,
            "POST",
            "/api/v1/files"
        ).count,
        static_cast<std::uint64_t>(1),
        "Upload route-counter test"
    );

    require_equal(
        find_route_counter(
            snapshot.requests_by_route,
            "GET",
            "/unknown"
        ).count,
        static_cast<std::uint64_t>(1),
        "Unknown route-counter test"
    );

    require_equal(
        find_status_counter(
            snapshot.responses_by_status,
            200
        ).count,
        static_cast<std::uint64_t>(1),
        "HTTP 200 counter test"
    );

    require_equal(
        find_status_counter(
            snapshot.responses_by_status,
            201
        ).count,
        static_cast<std::uint64_t>(1),
        "HTTP 201 counter test"
    );

    require_equal(
        find_status_counter(
            snapshot.responses_by_status,
            404
        ).count,
        static_cast<std::uint64_t>(1),
        "HTTP 404 counter test"
    );

    require_sorted_dimensions(
        snapshot
    );
}

void test_request_gauge_protection()
{
    MetricsRegistry registry;

    /*
     * Finishing without a matching start must not underflow.
     */
    registry.request_finished(
        "GET",
        "/unmatched",
        500,
        std::chrono::microseconds{
            -10
        }
    );

    const MetricsSnapshot snapshot =
        registry.snapshot();

    require_equal(
        snapshot.requests_total,
        static_cast<std::uint64_t>(0),
        "Unmatched request-total test"
    );

    require_equal(
        snapshot.requests_in_flight,
        static_cast<std::uint64_t>(0),
        "Request gauge-underflow test"
    );

    require_equal(
        snapshot.requests_failed,
        static_cast<std::uint64_t>(1),
        "Unmatched failed-request test"
    );

    require_equal(
        snapshot
            .request_latency_microseconds_total,
        static_cast<std::uint64_t>(0),
        "Negative latency normalization test"
    );
}

void test_storage_operation_metrics()
{
    MetricsRegistry registry;

    registry.upload_finished(
        1024,
        true
    );

    registry.upload_finished(
        9999,
        false
    );

    registry.restoration_finished(
        2048,
        true
    );

    registry.restoration_finished(
        7777,
        false
    );

    registry.verification_finished(
        4096,
        true
    );

    registry.verification_finished(
        8888,
        false
    );

    const MetricsSnapshot snapshot =
        registry.snapshot();

    require_equal(
        snapshot.uploads_total,
        static_cast<std::uint64_t>(2),
        "Upload total test"
    );

    require_equal(
        snapshot.uploads_succeeded,
        static_cast<std::uint64_t>(1),
        "Upload success test"
    );

    require_equal(
        snapshot.uploads_failed,
        static_cast<std::uint64_t>(1),
        "Upload failure test"
    );

    require_equal(
        snapshot.upload_bytes_processed,
        static_cast<std::uint64_t>(1024),
        "Successful upload-byte test"
    );

    require_equal(
        snapshot.restorations_total,
        static_cast<std::uint64_t>(2),
        "Restoration total test"
    );

    require_equal(
        snapshot.restorations_succeeded,
        static_cast<std::uint64_t>(1),
        "Restoration success test"
    );

    require_equal(
        snapshot.restorations_failed,
        static_cast<std::uint64_t>(1),
        "Restoration failure test"
    );

    require_equal(
        snapshot.restoration_bytes_written,
        static_cast<std::uint64_t>(2048),
        "Successful restoration-byte test"
    );

    require_equal(
        snapshot.verifications_total,
        static_cast<std::uint64_t>(2),
        "Verification total test"
    );

    require_equal(
        snapshot.verifications_succeeded,
        static_cast<std::uint64_t>(1),
        "Verification success test"
    );

    require_equal(
        snapshot.verifications_failed,
        static_cast<std::uint64_t>(1),
        "Verification failure test"
    );

    require_equal(
        snapshot.verification_bytes_processed,
        static_cast<std::uint64_t>(4096),
        "Successful verification-byte test"
    );
}

void test_storage_catalog_gauges()
{
    MetricsRegistry registry;

    registry.set_storage_catalog(
        10,
        8,
        2,
        44,
        3
    );

    MetricsSnapshot snapshot =
        registry.snapshot();

    require_equal(
        snapshot.storage_manifests,
        static_cast<std::uint64_t>(10),
        "Initial manifest-gauge test"
    );

    require_equal(
        snapshot.storage_complete_manifests,
        static_cast<std::uint64_t>(8),
        "Initial complete-manifest gauge test"
    );

    require_equal(
        snapshot.storage_incomplete_manifests,
        static_cast<std::uint64_t>(2),
        "Initial incomplete-manifest gauge test"
    );

    require_equal(
        snapshot.storage_chunks,
        static_cast<std::uint64_t>(44),
        "Initial chunk-gauge test"
    );

    require_equal(
        snapshot.storage_missing_chunks,
        static_cast<std::uint64_t>(3),
        "Initial missing-chunk gauge test"
    );

    /*
     * Gauges must be replaceable rather than cumulative.
     */
    registry.set_storage_catalog(
        4,
        4,
        0,
        12,
        0
    );

    snapshot =
        registry.snapshot();

    require_equal(
        snapshot.storage_manifests,
        static_cast<std::uint64_t>(4),
        "Replaced manifest-gauge test"
    );

    require_equal(
        snapshot.storage_complete_manifests,
        static_cast<std::uint64_t>(4),
        "Replaced complete-manifest gauge test"
    );

    require_equal(
        snapshot.storage_incomplete_manifests,
        static_cast<std::uint64_t>(0),
        "Replaced incomplete-manifest gauge test"
    );

    require_equal(
        snapshot.storage_chunks,
        static_cast<std::uint64_t>(12),
        "Replaced chunk-gauge test"
    );

    require_equal(
        snapshot.storage_missing_chunks,
        static_cast<std::uint64_t>(0),
        "Replaced missing-chunk gauge test"
    );
}

void test_concurrent_metrics_updates()
{
    constexpr std::size_t thread_count =
        8;

    constexpr std::size_t requests_per_thread =
        1000;

    MetricsRegistry registry;

    std::barrier start_barrier{
        static_cast<std::ptrdiff_t>(
            thread_count
        )
    };

    std::barrier opened_barrier{
        static_cast<std::ptrdiff_t>(
            thread_count
        )
    };

    std::atomic<bool> workers_finished{
        false
    };

    std::exception_ptr observer_exception;

    std::thread observer{
        [
            &registry,
            &workers_finished,
            &observer_exception
        ]()
        {
            try
            {
                while (
                    !workers_finished.load(
                        std::memory_order_acquire
                    )
                )
                {
                    const MetricsSnapshot snapshot =
                        registry.snapshot();

                    const std::uint64_t
                        maximum_connections =
                            static_cast<std::uint64_t>(
                                thread_count
                            );

                    const std::uint64_t
                        maximum_requests =
                            static_cast<std::uint64_t>(
                                thread_count
                                * requests_per_thread
                            );

                    /*
                     * Aggregate counters use independent relaxed
                     * atomics. During active writes, related values
                     * may represent adjacent instants. Concurrent
                     * snapshots therefore validate safe bounds.
                     *
                     * Exact counter relationships are checked after
                     * all worker threads have joined.
                     */
                    require_true(
                        snapshot.connections_total
                            <= maximum_connections,
                        "Concurrent connection-total bound test"
                    );

                    require_true(
                        snapshot.connections_active
                            <= maximum_connections,
                        "Concurrent active-connection bound test"
                    );

                    require_true(
                        snapshot.connections_peak
                            <= maximum_connections,
                        "Concurrent peak-connection bound test"
                    );

                    require_true(
                        snapshot.requests_total
                            <= maximum_requests,
                        "Concurrent request-total bound test"
                    );

                    require_true(
                        snapshot.requests_in_flight
                            <= maximum_requests,
                        "Concurrent in-flight bound test"
                    );

                    require_true(
                        snapshot.requests_succeeded
                            <= maximum_requests,
                        "Concurrent request-success bound test"
                    );

                    require_true(
                        snapshot.requests_failed
                            <= maximum_requests,
                        "Concurrent request-failure bound test"
                    );

                    std::this_thread::yield();
                }
            }
            catch (...)
            {
                observer_exception =
                    std::current_exception();
            }
        }
    };

    std::vector<std::thread> workers;

    workers.reserve(
        thread_count
    );

    for (
        std::size_t thread_index = 0;
        thread_index < thread_count;
        ++thread_index
    )
    {
        workers.emplace_back(
            [
                &registry,
                &start_barrier,
                &opened_barrier
            ]()
            {
                start_barrier.arrive_and_wait();

                registry.connection_opened();

                /*
                 * No worker may close until every connection has
                 * opened, making the expected peak deterministic.
                 */
                opened_barrier.arrive_and_wait();

                for (
                    std::size_t request_index = 0;
                    request_index
                        < requests_per_thread;
                    ++request_index
                )
                {
                    const bool is_get =
                        (
                            request_index % 2
                            == 0
                        );

                    const bool is_failure =
                        (
                            request_index % 5
                            == 0
                        );

                    registry.request_started();

                    registry.request_finished(
                        is_get
                            ? "GET"
                            : "POST",
                        is_get
                            ? "/api/v1/health"
                            : "/api/v1/files",
                        is_failure
                            ? 500U
                            : 200U,
                        std::chrono::microseconds{
                            static_cast<
                                std::chrono::
                                    microseconds::rep
                            >(
                                request_index % 100
                                + 1
                            )
                        }
                    );
                }

                registry.upload_finished(
                    1024,
                    true
                );

                registry.upload_finished(
                    0,
                    false
                );

                registry.restoration_finished(
                    2048,
                    true
                );

                registry.verification_finished(
                    4096,
                    true
                );

                registry.connection_closed();
            }
        );
    }

    for (
        std::thread& worker :
        workers
    )
    {
        worker.join();
    }

    workers_finished.store(
        true,
        std::memory_order_release
    );

    observer.join();

    if (observer_exception)
    {
        std::rethrow_exception(
            observer_exception
        );
    }

    const MetricsSnapshot snapshot =
        registry.snapshot();

    const std::uint64_t expected_requests =
        static_cast<std::uint64_t>(
            thread_count
            * requests_per_thread
        );

    const std::uint64_t expected_failures =
        static_cast<std::uint64_t>(
            thread_count
            * (
                requests_per_thread / 5
            )
        );

    const std::uint64_t expected_successes =
        expected_requests
        - expected_failures;

    const std::uint64_t expected_get_requests =
        static_cast<std::uint64_t>(
            thread_count
            * (
                requests_per_thread / 2
            )
        );

    const std::uint64_t expected_post_requests =
        expected_get_requests;

    /*
     * For 1..100 repeated ten times:
     * 10 * (100 * 101 / 2) = 50,500 microseconds
     * per worker.
     */
    constexpr std::uint64_t
        latency_per_thread =
            50500;

    require_equal(
        snapshot.connections_total,
        static_cast<std::uint64_t>(
            thread_count
        ),
        "Concurrent connection-total test"
    );

    require_equal(
        snapshot.connections_active,
        static_cast<std::uint64_t>(0),
        "Concurrent active-connection test"
    );

    require_equal(
        snapshot.connections_peak,
        static_cast<std::uint64_t>(
            thread_count
        ),
        "Concurrent peak-connection test"
    );

    require_equal(
        snapshot.requests_total,
        expected_requests,
        "Concurrent request-total test"
    );

    require_equal(
        snapshot.requests_in_flight,
        static_cast<std::uint64_t>(0),
        "Concurrent in-flight completion test"
    );

    require_equal(
        snapshot.requests_succeeded,
        expected_successes,
        "Concurrent successful-request test"
    );

    require_equal(
        snapshot.requests_failed,
        expected_failures,
        "Concurrent failed-request test"
    );

    require_equal(
        snapshot
            .request_latency_microseconds_total,
        latency_per_thread
            * static_cast<std::uint64_t>(
                thread_count
            ),
        "Concurrent latency-total test"
    );

    require_equal(
        snapshot
            .request_latency_microseconds_max,
        static_cast<std::uint64_t>(100),
        "Concurrent latency-maximum test"
    );

    require_equal(
        find_named_counter(
            snapshot.requests_by_method,
            "GET"
        ).count,
        expected_get_requests,
        "Concurrent GET-counter test"
    );

    require_equal(
        find_named_counter(
            snapshot.requests_by_method,
            "POST"
        ).count,
        expected_post_requests,
        "Concurrent POST-counter test"
    );

    require_equal(
        find_route_counter(
            snapshot.requests_by_route,
            "GET",
            "/api/v1/health"
        ).count,
        expected_get_requests,
        "Concurrent health-route test"
    );

    require_equal(
        find_route_counter(
            snapshot.requests_by_route,
            "POST",
            "/api/v1/files"
        ).count,
        expected_post_requests,
        "Concurrent upload-route test"
    );

    require_equal(
        find_status_counter(
            snapshot.responses_by_status,
            200
        ).count,
        expected_successes,
        "Concurrent HTTP 200 test"
    );

    require_equal(
        find_status_counter(
            snapshot.responses_by_status,
            500
        ).count,
        expected_failures,
        "Concurrent HTTP 500 test"
    );

    require_equal(
        snapshot.uploads_total,
        static_cast<std::uint64_t>(
            thread_count * 2
        ),
        "Concurrent upload-total test"
    );

    require_equal(
        snapshot.uploads_succeeded,
        static_cast<std::uint64_t>(
            thread_count
        ),
        "Concurrent upload-success test"
    );

    require_equal(
        snapshot.uploads_failed,
        static_cast<std::uint64_t>(
            thread_count
        ),
        "Concurrent upload-failure test"
    );

    require_equal(
        snapshot.upload_bytes_processed,
        static_cast<std::uint64_t>(
            thread_count * 1024
        ),
        "Concurrent upload-byte test"
    );

    require_equal(
        snapshot.restorations_total,
        static_cast<std::uint64_t>(
            thread_count
        ),
        "Concurrent restoration-total test"
    );

    require_equal(
        snapshot.restoration_bytes_written,
        static_cast<std::uint64_t>(
            thread_count * 2048
        ),
        "Concurrent restoration-byte test"
    );

    require_equal(
        snapshot.verifications_total,
        static_cast<std::uint64_t>(
            thread_count
        ),
        "Concurrent verification-total test"
    );

    require_equal(
        snapshot.verification_bytes_processed,
        static_cast<std::uint64_t>(
            thread_count * 4096
        ),
        "Concurrent verification-byte test"
    );

    require_sorted_dimensions(
        snapshot
    );
}

}

int main()
{
    try
    {
        test_initial_snapshot();

        std::cout
            << "[PASS] Metrics initial snapshot\n";

        test_connection_lifecycle();

        std::cout
            << "[PASS] Metrics connection lifecycle\n";

        test_request_metrics();

        std::cout
            << "[PASS] Metrics request counters\n";

        test_request_gauge_protection();

        std::cout
            << "[PASS] Metrics request gauge protection\n";

        test_storage_operation_metrics();

        std::cout
            << "[PASS] Metrics storage operations\n";

        test_storage_catalog_gauges();

        std::cout
            << "[PASS] Metrics storage catalog gauges\n";

        test_concurrent_metrics_updates();

        std::cout
            << "[PASS] Metrics concurrent updates\n";

        std::cout
            << "All NexusFS metrics registry tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "[FAIL] "
            << error.what()
            << '\n';

        return 1;
    }
}