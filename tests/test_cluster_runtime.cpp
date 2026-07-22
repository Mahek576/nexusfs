#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/cluster/heartbeat_scheduler.hpp"
#include "nexusfs/cluster/peer_transport.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{

namespace asio =
    boost::asio;

namespace beast_http =
    boost::beast::http;

using Tcp =
    asio::ip::tcp;

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
                "nexusfs-cluster-runtime-tests-"
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

std::uint16_t reserve_port()
{
    asio::io_context context{
        1
    };

    Tcp::acceptor acceptor{
        context,
        Tcp::endpoint{
            asio::ip::make_address(
                "127.0.0.1"
            ),
            0
        }
    };

    const std::uint16_t port =
        acceptor
            .local_endpoint()
            .port();

    acceptor.close();

    return port;
}

void write_configuration(
    const std::filesystem::path& path,
    const std::string& local_node_id,
    std::uint16_t local_port,
    std::uint16_t unavailable_port
)
{
    (void)local_node_id;

    const nlohmann::ordered_json payload = {
        {
            "schema_version",
            1
        },
        {
            "cluster_id",
            "cluster-runtime-test"
        },
        {
            "advertise_address",
            "127.0.0.1"
        },
        {
            "advertise_port",
            local_port
        },
        {
            "heartbeat_interval_ms",
            100
        },
        {
            "failure_timeout_ms",
            500
        },
        {
            "replication_factor",
            2
        },
        {
            "strict_replication",
            true
        },
        {
            "peers",
            {
                {
                    {
                        "node_id",
                        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
                    },
                    {
                        "address",
                        "127.0.0.1"
                    },
                    {
                        "port",
                        unavailable_port
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
            "Failed to create runtime test configuration."
        );
    }

    output
        << payload.dump(2)
        << '\n';

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write runtime test configuration."
        );
    }
}

std::shared_ptr<
    nexusfs::cluster::ClusterNodeFoundation
>
make_cluster(
    const std::filesystem::path& root
)
{
    const std::uint16_t local_port =
        reserve_port();

    const std::uint16_t unavailable_port =
        reserve_port();

    const auto initial =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    root,
                    "127.0.0.1",
                    local_port
                );

    write_configuration(
        initial->cluster_directory()
            / "cluster.json",
        initial->identity().node_id,
        local_port,
        unavailable_port
    );

    return nexusfs::cluster::
        ClusterNodeFoundation::
            load_or_create(
                root,
                "127.0.0.1",
                local_port
            );
}

void wait_for_heartbeat_failure(
    const std::shared_ptr<
        nexusfs::observability::
            MetricsRegistry
    >& metrics
)
{
    const auto deadline =
        std::chrono::steady_clock::now()
        + std::chrono::seconds{
            5
        };

    while (
        metrics->snapshot()
            .heartbeat_attempts_failed == 0
    )
    {
        if (
            std::chrono::steady_clock::now()
            >= deadline
        )
        {
            throw std::runtime_error(
                "Heartbeat scheduler failure observation "
                "timed out."
            );
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds{
                10
            }
        );
    }
}

void test_scheduler_failure_and_shutdown()
{
    TemporaryDirectory directory;

    const auto cluster =
        make_cluster(
            directory.path()
            / "scheduler-node"
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

    nexusfs::cluster::HeartbeatScheduler
        scheduler{
            cluster,
            metrics,
            logger,
            std::chrono::milliseconds{
                100
            },
            std::chrono::milliseconds{
                200
            }
        };

    scheduler.start();

    wait_for_heartbeat_failure(
        metrics
    );

    require_true(
        scheduler.is_running(),
        "Heartbeat scheduler running-state test"
    );

    scheduler.stop();

    require_true(
        !scheduler.is_running(),
        "Heartbeat scheduler stopped-state test"
    );

    scheduler.stop();

    const auto snapshot =
        metrics->snapshot();

    require_true(
        snapshot.heartbeat_attempts_total >= 1,
        "Heartbeat attempt metric test"
    );

    require_true(
        snapshot.heartbeat_attempts_failed >= 1,
        "Heartbeat failure metric test"
    );

    require_equal(
        snapshot.heartbeat_attempts_succeeded,
        static_cast<std::uint64_t>(0),
        "Heartbeat success isolation test"
    );

    const std::string output =
        logs.str();

    require_true(
        output.find(
            "heartbeat_scheduler_started"
        ) != std::string::npos,
        "Heartbeat scheduler start-log test"
    );

    require_true(
        output.find(
            "peer_heartbeat_failed"
        ) != std::string::npos,
        "Heartbeat scheduler failure-log test"
    );

    require_true(
        output.find(
            "heartbeat_scheduler_stopped"
        ) != std::string::npos,
        "Heartbeat scheduler stop-log test"
    );
}

void test_replication_metrics_and_logging()
{
    TemporaryDirectory directory;

    const auto cluster =
        make_cluster(
            directory.path()
            / "replication-node"
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

    nexusfs::cluster::
        ReplicationCoordinator coordinator{
            cluster,
            std::chrono::milliseconds{
                200
            },
            metrics,
            logger
        };

    const std::vector<std::uint8_t> data{
        10,
        20,
        30,
        40,
        50
    };

    const std::string hash =
        nexusfs::storage::
            Sha256Hasher::hash(
                std::span<
                    const std::uint8_t
                >{
                    data.data(),
                    data.size()
                }
            );

    const auto report =
        coordinator.replicate_chunk(
            hash,
            data,
            2
        );

    require_true(
        !report.satisfied,
        "Replication failure-report test"
    );

    const auto snapshot =
        metrics->snapshot();

    require_equal(
        snapshot.replication_chunks_total,
        static_cast<std::uint64_t>(1),
        "Replication total metric test"
    );

    require_equal(
        snapshot.replication_chunks_failed,
        static_cast<std::uint64_t>(1),
        "Replication failure metric test"
    );

    require_equal(
        snapshot.replication_chunks_satisfied,
        static_cast<std::uint64_t>(0),
        "Replication satisfied isolation test"
    );

    require_equal(
        snapshot.replication_remote_acknowledgements,
        static_cast<std::uint64_t>(0),
        "Replication acknowledgement metric test"
    );

    require_equal(
        snapshot.replication_remote_failures,
        static_cast<std::uint64_t>(1),
        "Replication remote-failure metric test"
    );

    require_true(
        logs.str().find(
            "chunk_replication_completed"
        ) != std::string::npos,
        "Replication structured-log test"
    );
}

void test_cluster_metrics_endpoint()
{
    TemporaryDirectory directory;

    const auto metrics =
        std::make_shared<
            nexusfs::observability::
                MetricsRegistry
        >();

    metrics->record_heartbeat_attempt(
        true
    );

    metrics->record_heartbeat_attempt(
        false
    );

    metrics->record_replication_result(
        2,
        0,
        true
    );

    metrics->record_replication_result(
        0,
        1,
        false
    );

    const auto service =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            directory.path()
            / "endpoint-storage",
            1024
        );

    const nexusfs::http::HttpRouter router{
        service,
        metrics
    };

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::get,
        "/api/v1/metrics",
        11
    };

    request.keep_alive(
        false
    );

    const auto response =
        router.route(
            request
        );

    require_equal(
        response.result(),
        beast_http::status::ok,
        "Cluster metrics endpoint status test"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    require_equal(
        payload
            .at("cluster_transport")
            .at("heartbeats")
            .at("attempted")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(2),
        "Heartbeat endpoint metric test"
    );

    require_equal(
        payload
            .at("cluster_transport")
            .at("replication")
            .at("chunks_satisfied")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Replication endpoint success test"
    );

    require_equal(
        payload
            .at("cluster_transport")
            .at("replication")
            .at("chunks_failed")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Replication endpoint failure test"
    );

    require_equal(
        payload
            .at("cluster_transport")
            .at("replication")
            .at("remote_acknowledgements")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(2),
        "Replication endpoint acknowledgement test"
    );
}

}

int main()
{
    try
    {
        test_scheduler_failure_and_shutdown();

        std::cout
            << "[PASS] Heartbeat scheduler failure and shutdown\n";

        test_replication_metrics_and_logging();

        std::cout
            << "[PASS] Replication metrics and logging\n";

        test_cluster_metrics_endpoint();

        std::cout
            << "[PASS] Cluster transport metrics endpoint\n";

        std::cout
            << "All NexusFS cluster runtime tests passed.\n";

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
