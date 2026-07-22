#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace
{

namespace beast_http =
    boost::beast::http;

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        static std::atomic<std::uint64_t>
            sequence{
                0
            };

        path_ =
            std::filesystem::temp_directory_path()
            / (
                "nexusfs-http-cluster-tests-"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count()
                )
                + "-"
                + std::to_string(
                    sequence.fetch_add(
                        1,
                        std::memory_order_relaxed
                    )
                )
            );

        std::filesystem::create_directories(
            path_
        );
    }

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

void write_cluster_configuration(
    const std::filesystem::path& path
)
{
    const nlohmann::ordered_json payload = {
        {
            "schema_version",
            1
        },
        {
            "cluster_id",
            "http-cluster-test"
        },
        {
            "advertise_address",
            "127.0.0.1"
        },
        {
            "advertise_port",
            8200
        },
        {
            "heartbeat_interval_ms",
            1000
        },
        {
            "failure_timeout_ms",
            5000
        },
        {
            "peers",
            {
                {
                    {
                        "node_id",
                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                    },
                    {
                        "address",
                        "127.0.0.1"
                    },
                    {
                        "port",
                        8201
                    }
                }
            }
        }
    };

    std::ofstream output{
        path,
        std::ios::binary
            | std::ios::trunc
    };

    if (!output.is_open())
    {
        throw std::runtime_error(
            "Failed to create HTTP cluster configuration."
        );
    }

    output
        << payload.dump(2)
        << '\n';

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write HTTP cluster configuration."
        );
    }
}

nexusfs::http::HttpRouter::Request
make_request(
    beast_http::verb method,
    std::string target,
    std::string body = {}
)
{
    nexusfs::http::HttpRouter::Request request{
        method,
        std::move(target),
        11
    };

    request.set(
        beast_http::field::host,
        "127.0.0.1"
    );

    if (!body.empty())
    {
        request.set(
            beast_http::field::content_type,
            "application/json"
        );
    }

    request.body() =
        std::move(body);

    request.keep_alive(
        false
    );

    request.prepare_payload();

    return request;
}

void test_cluster_http_endpoints()
{
    TemporaryDirectory directory;

    const auto initial_cluster =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    directory.path(),
                    "127.0.0.1",
                    8200
                );

    write_cluster_configuration(
        initial_cluster->cluster_directory()
        / "cluster.json"
    );

    const auto cluster =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    directory.path(),
                    "127.0.0.1",
                    8200
                );

    const auto service =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            directory.path(),
            1024
        );

    const auto metrics =
        std::make_shared<
            nexusfs::observability::
                MetricsRegistry
        >();

    std::ostringstream logs;

    const auto logger =
        std::make_shared<
            nexusfs::observability::
                JsonLogger
        >(
            &logs
        );

    const nexusfs::http::HttpRouter router{
        service,
        metrics,
        logger,
        cluster
    };

    const auto initial_response =
        router.route(
            make_request(
                beast_http::verb::get,
                "/api/v1/cluster"
            )
        );

    require_equal(
        initial_response.result(),
        beast_http::status::ok,
        "Cluster status response test"
    );

    const nlohmann::json initial_payload =
        nlohmann::json::parse(
            initial_response.body()
        );

    require_equal(
        initial_payload
            .at("cluster_id")
            .get<std::string>(),
        std::string{
            "http-cluster-test"
        },
        "Cluster status ID test"
    );

    require_equal(
        initial_payload
            .at("summary")
            .at("configured_peers")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Cluster configured-peer test"
    );

    require_equal(
        initial_payload
            .at("summary")
            .at("unknown")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Cluster initial peer-state test"
    );

    const nexusfs::cluster::HeartbeatMessage
        heartbeat{
            "http-cluster-test",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "127.0.0.1",
            8201,
            123456789
        };

    const auto heartbeat_response =
        router.route(
            make_request(
                beast_http::verb::post,
                "/api/v1/cluster/heartbeat",
                nexusfs::cluster::
                    ClusterNodeFoundation::
                        encode_heartbeat(
                            heartbeat
                        )
            )
        );

    require_equal(
        heartbeat_response.result(),
        beast_http::status::ok,
        "Heartbeat acceptance status test"
    );

    const auto updated_response =
        router.route(
            make_request(
                beast_http::verb::get,
                "/api/v1/cluster"
            )
        );

    const nlohmann::json updated_payload =
        nlohmann::json::parse(
            updated_response.body()
        );

    require_equal(
        updated_payload
            .at("summary")
            .at("healthy")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Heartbeat health-transition test"
    );

    require_equal(
        updated_payload
            .at("peers")
            .at(0)
            .at("successful_heartbeats")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Heartbeat counter test"
    );

    require_true(
        logs.str().find(
            "cluster_heartbeat_received"
        ) != std::string::npos,
        "Heartbeat structured-log test"
    );
}

void test_cluster_http_validation()
{
    TemporaryDirectory directory;

    const auto cluster =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    directory.path(),
                    "127.0.0.1",
                    8300
                );

    const auto service =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            directory.path(),
            1024
        );

    const auto metrics =
        std::make_shared<
            nexusfs::observability::
                MetricsRegistry
        >();

    const auto logger =
        std::make_shared<
            nexusfs::observability::
                JsonLogger
        >();

    const nexusfs::http::HttpRouter router{
        service,
        metrics,
        logger,
        cluster
    };

    const auto malformed_response =
        router.route(
            make_request(
                beast_http::verb::post,
                "/api/v1/cluster/heartbeat",
                "{invalid-json"
            )
        );

    require_equal(
        malformed_response.result(),
        beast_http::status::bad_request,
        "Malformed heartbeat HTTP test"
    );

    const auto cluster_method_response =
        router.route(
            make_request(
                beast_http::verb::post,
                "/api/v1/cluster"
            )
        );

    require_equal(
        cluster_method_response.result(),
        beast_http::status::
            method_not_allowed,
        "Cluster status method test"
    );

    require_equal(
        cluster_method_response[
            beast_http::field::allow
        ],
        std::string_view{
            "GET"
        },
        "Cluster status Allow-header test"
    );

    const auto heartbeat_method_response =
        router.route(
            make_request(
                beast_http::verb::get,
                "/api/v1/cluster/heartbeat"
            )
        );

    require_equal(
        heartbeat_method_response.result(),
        beast_http::status::
            method_not_allowed,
        "Heartbeat method test"
    );

    require_equal(
        router.normalized_route(
            make_request(
                beast_http::verb::get,
                "/api/v1/cluster"
            )
        ),
        std::string_view{
            "/api/v1/cluster"
        },
        "Cluster normalized-route test"
    );

    require_equal(
        router.normalized_route(
            make_request(
                beast_http::verb::post,
                "/api/v1/cluster/heartbeat"
            )
        ),
        std::string_view{
            "/api/v1/cluster/heartbeat"
        },
        "Heartbeat normalized-route test"
    );
}

}

int main()
{
    try
    {
        test_cluster_http_endpoints();

        std::cout
            << "[PASS] Cluster HTTP status and heartbeat\n";

        test_cluster_http_validation();

        std::cout
            << "[PASS] Cluster HTTP validation\n";

        std::cout
            << "All NexusFS cluster HTTP tests passed.\n";

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
