#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"
#include "nexusfs/observability/metrics_registry.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

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
                "nexusfs-metrics-endpoint-tests-"
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
                "Failed to create metrics endpoint "
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

const HttpRouteCounter&
find_route_counter(
    const MetricsSnapshot& snapshot,
    const std::string& method,
    const std::string& route
)
{
    for (
        const HttpRouteCounter& counter :
        snapshot.requests_by_route
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
    const MetricsSnapshot& snapshot,
    unsigned int status_code
)
{
    for (
        const HttpStatusCounter& counter :
        snapshot.responses_by_status
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

class RunningServer final
{
public:
    explicit RunningServer(
        nexusfs::http::HttpServer& server
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
                        const std::lock_guard lock{
                            exception_mutex_
                        };

                        exception_ =
                            std::current_exception();
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
                "Metrics endpoint server startup"
            );

            rethrow_if_failed();

            require_true(
                server_.is_running(),
                "Metrics endpoint server running test"
            );

            require_true(
                server_.port() != 0,
                "Metrics endpoint ephemeral-port test"
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

        return exception_ != nullptr;
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

    void stop_and_join_noexcept() noexcept
    {
        server_.stop();

        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    nexusfs::http::HttpServer& server_;
    std::thread thread_;

    mutable std::mutex exception_mutex_;
    std::exception_ptr exception_;
};

nexusfs::http::HttpRouter::Response
send_request(
    std::uint16_t port,
    beast_http::verb method,
    std::string_view target,
    std::string body = {},
    std::string content_type = {},
    std::string upload_filename = {}
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
        "NexusFS metrics endpoint tests"
    );

    if (!content_type.empty())
    {
        request.set(
            beast_http::field::content_type,
            content_type
        );
    }

    if (!upload_filename.empty())
    {
        request.set(
            "X-NexusFS-Filename",
            upload_filename
        );
    }

    request.body() =
        std::move(body);

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

    boost::system::error_code ignored_error;

    socket.shutdown(
        Tcp::socket::shutdown_both,
        ignored_error
    );

    socket.close(
        ignored_error
    );

    return response;
}

std::string make_binary_payload(
    std::size_t size
)
{
    std::string payload(
        size,
        '\0'
    );

    for (
        std::size_t index = 0;
        index < payload.size();
        ++index
    )
    {
        const std::size_t block_number =
            index / 256;

        payload[index] =
            static_cast<char>(
                (
                    index * 37
                    + block_number * 17
                    + 19
                )
                % 256
            );
    }

    return payload;
}

nlohmann::json parse_json_response(
    const nexusfs::http::
        HttpRouter::Response& response,
    const std::string& test_name
)
{
    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body(),
            nullptr,
            false
        );

    require_true(
        !payload.is_discarded(),
        test_name
    );

    require_true(
        payload.is_object(),
        test_name
    );

    return payload;
}

void wait_for_idle_metrics(
    const std::shared_ptr<
        MetricsRegistry
    >& metrics
)
{
    wait_until(
        [metrics]()
        {
            const MetricsSnapshot snapshot =
                metrics->snapshot();

            return (
                snapshot.connections_active
                    == 0
                && snapshot.requests_in_flight
                    == 0
            );
        },
        "Metrics endpoint session cleanup"
    );
}

void test_metrics_endpoint_and_operations()
{
    TemporaryDirectory directory;

    const auto service =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            directory.path()
                / "storage",
            1024
        );

    const auto metrics =
        std::make_shared<
            MetricsRegistry
        >();

    const nexusfs::http::HttpRouter router{
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
        "Metrics endpoint health status test"
    );

    wait_for_idle_metrics(
        metrics
    );

    const std::string binary_payload =
        make_binary_payload(
            2500
        );

    const auto upload_response =
        send_request(
            server.port(),
            beast_http::verb::post,
            "/api/v1/files",
            binary_payload,
            "application/octet-stream",
            "metrics.bin"
        );

    require_equal(
        upload_response.result(),
        beast_http::status::created,
        "Metrics endpoint upload status test"
    );

    const nlohmann::json upload_payload =
        parse_json_response(
            upload_response,
            "Metrics endpoint upload JSON test"
        );

    const std::string manifest_id =
        upload_payload
            .at("stored_file")
            .at("manifest_id")
            .get<std::string>();

    require_equal(
        upload_payload
            .at("stored_file")
            .at("chunk_count")
            .get<std::size_t>(),
        static_cast<std::size_t>(3),
        "Metrics endpoint upload chunk-count test"
    );

    wait_for_idle_metrics(
        metrics
    );

    const auto verification_response =
        send_request(
            server.port(),
            beast_http::verb::post,
            "/api/v1/files/"
                + manifest_id
                + "/verify"
        );

    require_equal(
        verification_response.result(),
        beast_http::status::ok,
        "Metrics endpoint verification status test"
    );

    wait_for_idle_metrics(
        metrics
    );

    const std::filesystem::path restored_path =
        directory.path()
        / "restored"
        / "metrics.bin";

    const nlohmann::json restoration_request = {
        {
            "output_path",
            restored_path.string()
        }
    };

    const auto restoration_response =
        send_request(
            server.port(),
            beast_http::verb::post,
            "/api/v1/files/"
                + manifest_id
                + "/restore",
            restoration_request.dump(),
            "application/json"
        );

    require_equal(
        restoration_response.result(),
        beast_http::status::created,
        "Metrics endpoint restoration status test"
    );

    require_true(
        std::filesystem::exists(
            restored_path
        ),
        "Metrics endpoint restored-file test"
    );

    wait_for_idle_metrics(
        metrics
    );

    const auto metrics_response =
        send_request(
            server.port(),
            beast_http::verb::get,
            "/api/v1/metrics"
        );

    require_equal(
        metrics_response.result(),
        beast_http::status::ok,
        "Metrics endpoint GET status test"
    );

    require_equal(
        metrics_response[
            beast_http::field::content_type
        ],
        boost::beast::string_view{
            "application/json"
        },
        "Metrics endpoint content-type test"
    );

    const nlohmann::json endpoint_payload =
        parse_json_response(
            metrics_response,
            "Metrics endpoint JSON test"
        );

    require_equal(
        endpoint_payload
            .at("service")
            .get<std::string>(),
        std::string{
            "nexusfs"
        },
        "Metrics endpoint service test"
    );

    require_equal(
        endpoint_payload
            .at("api_version")
            .get<std::string>(),
        std::string{
            "v1"
        },
        "Metrics endpoint API-version test"
    );

    require_equal(
        endpoint_payload
            .at("scope")
            .get<std::string>(),
        std::string{
            "process_local"
        },
        "Metrics endpoint scope test"
    );

    /*
     * The metrics request has already started when its snapshot is
     * generated, but it has not yet completed.
     */
    require_equal(
        endpoint_payload
            .at("http")
            .at("requests")
            .at("total")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(5),
        "Metrics endpoint self-observed total test"
    );

    require_equal(
        endpoint_payload
            .at("http")
            .at("requests")
            .at("in_flight")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Metrics endpoint self-observed in-flight test"
    );

    require_equal(
        endpoint_payload
            .at("http")
            .at("requests")
            .at("succeeded")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(4),
        "Metrics endpoint completed-success test"
    );

    require_equal(
        endpoint_payload
            .at("http")
            .at("requests")
            .at("failed")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(0),
        "Metrics endpoint completed-failure test"
    );

    require_equal(
        endpoint_payload
            .at("operations")
            .at("uploads")
            .at("total")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Metrics endpoint upload-total test"
    );

    require_equal(
        endpoint_payload
            .at("operations")
            .at("uploads")
            .at("succeeded")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Metrics endpoint upload-success test"
    );

    require_equal(
        endpoint_payload
            .at("operations")
            .at("uploads")
            .at("bytes_processed")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(2500),
        "Metrics endpoint upload-byte test"
    );

    require_equal(
        endpoint_payload
            .at("operations")
            .at("verifications")
            .at("total")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Metrics endpoint verification-total test"
    );

    require_equal(
        endpoint_payload
            .at("operations")
            .at("verifications")
            .at("bytes_processed")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(2500),
        "Metrics endpoint verification-byte test"
    );

    require_equal(
        endpoint_payload
            .at("operations")
            .at("restorations")
            .at("total")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Metrics endpoint restoration-total test"
    );

    require_equal(
        endpoint_payload
            .at("operations")
            .at("restorations")
            .at("bytes_written")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(2500),
        "Metrics endpoint restoration-byte test"
    );

    require_equal(
        endpoint_payload
            .at("storage")
            .at("stored_manifests")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Metrics endpoint manifest-gauge test"
    );

    require_equal(
        endpoint_payload
            .at("storage")
            .at("complete_manifests")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Metrics endpoint complete-manifest test"
    );

    require_equal(
        endpoint_payload
            .at("storage")
            .at("incomplete_manifests")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(0),
        "Metrics endpoint incomplete-manifest test"
    );

    require_equal(
        endpoint_payload
            .at("storage")
            .at("chunk_references")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(3),
        "Metrics endpoint chunk-reference test"
    );

    require_equal(
        endpoint_payload
            .at("storage")
            .at("missing_chunk_references")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(0),
        "Metrics endpoint missing-reference test"
    );

    wait_for_idle_metrics(
        metrics
    );

    const MetricsSnapshot completed_snapshot =
        metrics->snapshot();

    require_equal(
        completed_snapshot.requests_total,
        static_cast<std::uint64_t>(5),
        "Completed metrics request-total test"
    );

    require_equal(
        completed_snapshot.requests_in_flight,
        static_cast<std::uint64_t>(0),
        "Completed metrics in-flight test"
    );

    require_equal(
        completed_snapshot.requests_succeeded,
        static_cast<std::uint64_t>(5),
        "Completed metrics success test"
    );

    require_equal(
        find_route_counter(
            completed_snapshot,
            "GET",
            "/api/v1/metrics"
        ).count,
        static_cast<std::uint64_t>(1),
        "Metrics endpoint route-counter test"
    );

    const auto method_response =
        send_request(
            server.port(),
            beast_http::verb::post,
            "/api/v1/metrics"
        );

    require_equal(
        method_response.result(),
        beast_http::status::
            method_not_allowed,
        "Metrics endpoint method rejection test"
    );

    require_equal(
        method_response[
            beast_http::field::allow
        ],
        boost::beast::string_view{
            "GET"
        },
        "Metrics endpoint Allow-header test"
    );

    wait_for_idle_metrics(
        metrics
    );

    const MetricsSnapshot final_snapshot =
        metrics->snapshot();

    require_equal(
        final_snapshot.requests_total,
        static_cast<std::uint64_t>(6),
        "Final metrics request-total test"
    );

    require_equal(
        final_snapshot.requests_succeeded,
        static_cast<std::uint64_t>(5),
        "Final metrics success-total test"
    );

    require_equal(
        final_snapshot.requests_failed,
        static_cast<std::uint64_t>(1),
        "Final metrics failure-total test"
    );

    require_equal(
        find_route_counter(
            final_snapshot,
            "POST",
            "/api/v1/metrics"
        ).count,
        static_cast<std::uint64_t>(1),
        "Metrics endpoint POST route-counter test"
    );

    require_equal(
        find_status_counter(
            final_snapshot,
            200
        ).count,
        static_cast<std::uint64_t>(3),
        "Final HTTP 200 counter test"
    );

    require_equal(
        find_status_counter(
            final_snapshot,
            201
        ).count,
        static_cast<std::uint64_t>(2),
        "Final HTTP 201 counter test"
    );

    require_equal(
        find_status_counter(
            final_snapshot,
            405
        ).count,
        static_cast<std::uint64_t>(1),
        "Final HTTP 405 counter test"
    );

    running_server.stop_and_join();

    require_true(
        !server.is_running(),
        "Metrics endpoint shutdown test"
    );
}

}

int main()
{
    try
    {
        test_metrics_endpoint_and_operations();

        std::cout
            << "[PASS] Metrics endpoint JSON and operation metrics\n";

        std::cout
            << "All NexusFS metrics endpoint tests passed.\n";

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