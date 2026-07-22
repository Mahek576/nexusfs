#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"
#include "nexusfs/observability/metrics_registry.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{

namespace asio =
    boost::asio;

namespace beast_http =
    boost::beast::http;

using Tcp =
    asio::ip::tcp;

using nexusfs::observability::
    HttpRouteCounter;

using nexusfs::observability::
    HttpStatusCounter;

using nexusfs::observability::
    MetricsRegistry;

using nexusfs::observability::
    MetricsSnapshot;

using nexusfs::observability::
    NamedCounter;

constexpr std::chrono::seconds
    operation_timeout{
        5
    };

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        static std::atomic<std::uint64_t>
            sequence{
                0
            };

        const auto timestamp =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();

        const std::uint64_t current_sequence =
            sequence.fetch_add(
                1,
                std::memory_order_relaxed
            );

        path_ =
            std::filesystem::
                temp_directory_path()
            / (
                "nexusfs-http-metrics-tests-"
                + std::to_string(
                    timestamp
                )
                + "-"
                + std::to_string(
                    current_sequence
                )
            );

        std::error_code directory_error;

        std::filesystem::
            create_directories(
                path_,
                directory_error
            );

        if (directory_error)
        {
            throw std::runtime_error(
                "Failed to create HTTP metrics "
                "test directory: "
                + directory_error.message()
            );
        }
    }

    TemporaryDirectory(
        const TemporaryDirectory&
    ) = delete;

    TemporaryDirectory& operator=(
        const TemporaryDirectory&
    ) = delete;

    ~TemporaryDirectory()
    {
        std::error_code cleanup_error;

        std::filesystem::remove_all(
            path_,
            cleanup_error
        );
    }

    [[nodiscard]]
    const std::filesystem::path&
    path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

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

template <
    typename Actual,
    typename Expected
>
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

template <typename Predicate>
void wait_until(
    Predicate&& predicate,
    const std::string& test_name
)
{
    const auto deadline =
        std::chrono::steady_clock::now()
        + operation_timeout;

    while (
        !std::forward<Predicate>(
            predicate
        )()
    )
    {
        if (
            std::chrono::steady_clock::now()
            >= deadline
        )
        {
            throw std::runtime_error(
                test_name
                + " timed out."
            );
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds{
                5
            }
        );
    }
}

const NamedCounter&
find_named_counter(
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

const HttpRouteCounter&
find_route_counter(
    const std::vector<
        HttpRouteCounter
    >& counters,
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

const HttpStatusCounter&
find_status_counter(
    const std::vector<
        HttpStatusCounter
    >& counters,
    unsigned int status_code
)
{
    for (
        const HttpStatusCounter& counter :
        counters
    )
    {
        if (
            counter.status_code
            == status_code
        )
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

std::shared_ptr<
    nexusfs::app::NexusFsService
>
create_service(
    const std::filesystem::path&
        storage_root
)
{
    return std::make_shared<
        nexusfs::app::NexusFsService
    >(
        storage_root,
        1024
    );
}

class RunningServer final
{
public:
    explicit RunningServer(
        nexusfs::http::HttpServer&
            server
    )
        : server_{
              server
          }
    {
        thread_ =
            std::thread{
                [this]()
                {
                    try
                    {
                        server_.run();
                    }
                    catch (...)
                    {
                        const std::lock_guard
                            lock{
                                exception_mutex_
                            };

                        exception_ =
                            std::
                                current_exception();
                    }
                }
            };

        try
        {
            wait_until(
                [this]()
                {
                    return (
                        server_.is_running()
                        || has_exception()
                    );
                },
                "HTTP metrics server startup"
            );

            rethrow_if_failed();

            require_true(
                server_.is_running(),
                "HTTP metrics server "
                "running-state test"
            );

            require_true(
                server_.port() != 0,
                "HTTP metrics server "
                "ephemeral-port test"
            );
        }
        catch (...)
        {
            server_.stop();

            if (thread_.joinable())
            {
                thread_.join();
            }

            throw;
        }
    }

    RunningServer(
        const RunningServer&
    ) = delete;

    RunningServer& operator=(
        const RunningServer&
    ) = delete;

    ~RunningServer()
    {
        stop_and_join_noexcept();
    }

    void stop_and_join()
    {
        server_.stop();

        if (thread_.joinable())
        {
            thread_.join();
        }

        rethrow_if_failed();
    }

private:
    [[nodiscard]]
    bool has_exception() const
    {
        const std::lock_guard lock{
            exception_mutex_
        };

        return (
            exception_ != nullptr
        );
    }

    void rethrow_if_failed() const
    {
        std::exception_ptr exception;

        {
            const std::lock_guard lock{
                exception_mutex_
            };

            exception =
                exception_;
        }

        if (exception)
        {
            std::rethrow_exception(
                exception
            );
        }
    }

    void stop_and_join_noexcept()
        noexcept
    {
        server_.stop();

        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    nexusfs::http::HttpServer&
        server_;

    std::thread thread_;

    mutable std::mutex
        exception_mutex_;

    std::exception_ptr exception_;
};

nexusfs::http::HttpRouter::Response
send_request(
    std::uint16_t port,
    beast_http::verb method,
    std::string_view target
)
{
    asio::io_context io_context{
        1
    };

    Tcp::socket socket{
        io_context
    };

    const Tcp::endpoint endpoint{
        asio::ip::make_address(
            "127.0.0.1"
        ),
        port
    };

    socket.connect(
        endpoint
    );

    nexusfs::http::
        HttpRouter::Request request{
            method,
            std::string{
                target
            },
            11
        };

    request.set(
        beast_http::field::host,
        "127.0.0.1"
    );

    request.set(
        beast_http::field::user_agent,
        "NexusFS HTTP metrics tests"
    );

    request.keep_alive(
        false
    );

    request.prepare_payload();

    beast_http::write(
        socket,
        request
    );

    boost::beast::flat_buffer buffer;

    nexusfs::http::
        HttpRouter::Response response;

    beast_http::read(
        socket,
        buffer,
        response
    );

    boost::system::error_code
        ignored_error;

    socket.shutdown(
        Tcp::socket::shutdown_both,
        ignored_error
    );

    socket.close(
        ignored_error
    );

    return response;
}

void test_router_constructor_validation()
{
    TemporaryDirectory directory;

    const auto service =
        create_service(
            directory.path()
            / "storage"
        );

    const auto metrics =
        std::make_shared<
            MetricsRegistry
        >();

    {
        nexusfs::http::HttpRouter
            router{
                service
            };

        const MetricsSnapshot snapshot =
            router.metrics_registry().
                snapshot();

        require_equal(
            snapshot.requests_total,
            static_cast<std::uint64_t>(
                0
            ),
            "Default router metrics "
            "registry test"
        );
    }

    {
        nexusfs::http::HttpRouter
            router{
                service,
                metrics
            };

        require_true(
            &router.metrics_registry()
                == metrics.get(),
            "Explicit shared metrics "
            "registry test"
        );
    }

    bool null_service_rejected =
        false;

    try
    {
        nexusfs::http::HttpRouter
            router{
                std::shared_ptr<
                    const nexusfs::app::
                        NexusFsService
                >{},
                metrics
            };

        (void)router;
    }
    catch (
        const std::invalid_argument&
    )
    {
        null_service_rejected =
            true;
    }

    require_true(
        null_service_rejected,
        "Null router service rejection test"
    );

    bool null_metrics_rejected =
        false;

    try
    {
        nexusfs::http::HttpRouter
            router{
                service,
                std::shared_ptr<
                    MetricsRegistry
                >{}
            };

        (void)router;
    }
    catch (
        const std::invalid_argument&
    )
    {
        null_metrics_rejected =
            true;
    }

    require_true(
        null_metrics_rejected,
        "Null metrics registry "
        "rejection test"
    );
}

void test_route_normalization()
{
    TemporaryDirectory directory;

    const auto service =
        create_service(
            directory.path()
            / "storage"
        );

    nexusfs::http::HttpRouter
        router{
            service
        };

    const std::string manifest_id(
        64,
        'a'
    );

    const auto require_route =
        [
            &router
        ](
            beast_http::verb method,
            const std::string& target,
            std::string_view expected_route,
            const std::string& test_name
        )
        {
            nexusfs::http::
                HttpRouter::Request request{
                    method,
                    target,
                    11
                };

            require_equal(
                router.normalized_route(
                    request
                ),
                expected_route,
                test_name
            );
        };

    require_route(
        beast_http::verb::get,
        "/api/v1/health",
        "/api/v1/health",
        "Health route normalization test"
    );

    require_route(
        beast_http::verb::get,
        "/api/v1/metrics",
        "/api/v1/metrics",
        "Metrics route normalization test"
    );

    require_route(
        beast_http::verb::get,
        "/api/v1/files",
        "/api/v1/files",
        "Files route normalization test"
    );

    require_route(
        beast_http::verb::get,
        "/api/v1/files/"
            + manifest_id,
        "/api/v1/files/{manifest_id}",
        "File-detail normalization test"
    );

    require_route(
        beast_http::verb::post,
        "/api/v1/files/"
            + manifest_id
            + "/verify",
        "/api/v1/files/{manifest_id}/verify",
        "Verification route "
        "normalization test"
    );

    require_route(
        beast_http::verb::post,
        "/api/v1/files/"
            + manifest_id
            + "/restore",
        "/api/v1/files/{manifest_id}/restore",
        "Restoration route "
        "normalization test"
    );

    require_route(
        beast_http::verb::get,
        "/api/v1/files/not-a-hash",
        "/unmatched",
        "Invalid manifest route "
        "normalization test"
    );

    require_route(
        beast_http::verb::get,
        "/api/v1/files/"
            + manifest_id
            + "/unexpected",
        "/unmatched",
        "Unknown file subroute "
        "normalization test"
    );

    require_route(
        beast_http::verb::get,
        "/api/v1/health?verbose=true",
        "/unmatched",
        "Query-string normalization test"
    );

    require_route(
        beast_http::verb::get,
        "/completely-unknown",
        "/unmatched",
        "Unknown route normalization test"
    );
}

void test_real_http_instrumentation()
{
    TemporaryDirectory directory;

    const auto service =
        create_service(
            directory.path()
            / "storage"
        );

    const auto metrics =
        std::make_shared<
            MetricsRegistry
        >();

    const nexusfs::http::HttpRouter
        router{
            service,
            metrics
        };

    nexusfs::http::HttpServer server{
        "127.0.0.1",
        0,
        router
    };

    RunningServer running_server{
        server
    };

    const auto health_response =
        send_request(
            server.port(),
            beast_http::verb::get,
            "/api/v1/health"
        );

    require_equal(
        health_response.result(),
        beast_http::status::ok,
        "Instrumented health status test"
    );

    const auto method_response =
        send_request(
            server.port(),
            beast_http::verb::post,
            "/api/v1/health"
        );

    require_equal(
        method_response.result(),
        beast_http::status::
            method_not_allowed,
        "Instrumented method status test"
    );

    const auto unknown_response =
        send_request(
            server.port(),
            beast_http::verb::get,
            "/unknown"
        );

    require_equal(
        unknown_response.result(),
        beast_http::status::not_found,
        "Instrumented unknown status test"
    );

    wait_until(
        [
            &server,
            &metrics
        ]()
        {
            const MetricsSnapshot snapshot =
                metrics->snapshot();

            return (
                server.active_session_count()
                    == 0
                && snapshot.connections_active
                    == 0
                && snapshot.requests_in_flight
                    == 0
                && snapshot.requests_total
                    == 3
            );
        },
        "HTTP metrics completion"
    );

    const MetricsSnapshot snapshot =
        metrics->snapshot();

    require_equal(
        snapshot.connections_total,
        static_cast<std::uint64_t>(
            3
        ),
        "Instrumented connection-total test"
    );

    require_equal(
        snapshot.connections_active,
        static_cast<std::uint64_t>(
            0
        ),
        "Instrumented active-connection test"
    );

    require_true(
        snapshot.connections_peak
            >= 1,
        "Instrumented peak-connection test"
    );

    require_equal(
        snapshot.requests_total,
        static_cast<std::uint64_t>(
            3
        ),
        "Instrumented request-total test"
    );

    require_equal(
        snapshot.requests_in_flight,
        static_cast<std::uint64_t>(
            0
        ),
        "Instrumented in-flight test"
    );

    require_equal(
        snapshot.requests_succeeded,
        static_cast<std::uint64_t>(
            1
        ),
        "Instrumented success-count test"
    );

    require_equal(
        snapshot.requests_failed,
        static_cast<std::uint64_t>(
            2
        ),
        "Instrumented failure-count test"
    );

    require_true(
        snapshot
            .request_latency_microseconds_total
            >=
        snapshot
            .request_latency_microseconds_max,
        "Instrumented latency relationship test"
    );

    require_equal(
        find_named_counter(
            snapshot.requests_by_method,
            "GET"
        ).count,
        static_cast<std::uint64_t>(
            2
        ),
        "Instrumented GET-counter test"
    );

    require_equal(
        find_named_counter(
            snapshot.requests_by_method,
            "POST"
        ).count,
        static_cast<std::uint64_t>(
            1
        ),
        "Instrumented POST-counter test"
    );

    require_equal(
        find_route_counter(
            snapshot.requests_by_route,
            "GET",
            "/api/v1/health"
        ).count,
        static_cast<std::uint64_t>(
            1
        ),
        "Instrumented health GET test"
    );

    require_equal(
        find_route_counter(
            snapshot.requests_by_route,
            "POST",
            "/api/v1/health"
        ).count,
        static_cast<std::uint64_t>(
            1
        ),
        "Instrumented health POST test"
    );

    require_equal(
        find_route_counter(
            snapshot.requests_by_route,
            "GET",
            "/unmatched"
        ).count,
        static_cast<std::uint64_t>(
            1
        ),
        "Instrumented unmatched-route test"
    );

    require_equal(
        find_status_counter(
            snapshot.responses_by_status,
            200
        ).count,
        static_cast<std::uint64_t>(
            1
        ),
        "Instrumented HTTP 200 test"
    );

    require_equal(
        find_status_counter(
            snapshot.responses_by_status,
            404
        ).count,
        static_cast<std::uint64_t>(
            1
        ),
        "Instrumented HTTP 404 test"
    );

    require_equal(
        find_status_counter(
            snapshot.responses_by_status,
            405
        ).count,
        static_cast<std::uint64_t>(
            1
        ),
        "Instrumented HTTP 405 test"
    );

    running_server.stop_and_join();

    require_true(
        !server.is_running(),
        "Instrumented server shutdown test"
    );
}

}

int main()
{
    try
    {
        test_router_constructor_validation();

        std::cout
            << "[PASS] HTTP metrics router ownership\n";

        test_route_normalization();

        std::cout
            << "[PASS] HTTP metrics route normalization\n";

        test_real_http_instrumentation();

        std::cout
            << "[PASS] HTTP metrics real-server instrumentation\n";

        std::cout
            << "All NexusFS HTTP metrics tests passed.\n";

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