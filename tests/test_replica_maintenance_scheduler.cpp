#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/cluster/replica_maintenance_scheduler.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/chunker.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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
                "nexusfs-replica-scheduler-tests-"
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

void write_binary_file(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& data
)
{
    std::ofstream output{
        path,
        std::ios::binary
            | std::ios::trunc
    };

    if (!output.is_open())
    {
        throw std::runtime_error(
            "Failed to create scheduler test file."
        );
    }

    if (!data.empty())
    {
        output.write(
            reinterpret_cast<const char*>(
                data.data()
            ),
            static_cast<std::streamsize>(
                data.size()
            )
        );
    }

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write scheduler test file."
        );
    }
}

void write_configuration(
    const std::filesystem::path& path,
    const std::string& cluster_id,
    std::uint16_t local_port,
    std::size_t replication_factor,
    std::uint64_t maintenance_interval_ms,
    const std::vector<
        nexusfs::cluster::PeerDefinition
    >& peers
)
{
    nlohmann::ordered_json peer_payload =
        nlohmann::ordered_json::array();

    for (const auto& peer : peers)
    {
        peer_payload.push_back(
            {
                {
                    "node_id",
                    peer.node_id
                },
                {
                    "address",
                    peer.address
                },
                {
                    "port",
                    peer.port
                }
            }
        );
    }

    const nlohmann::ordered_json payload = {
        {
            "schema_version",
            1
        },
        {
            "cluster_id",
            cluster_id
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
            replication_factor
        },
        {
            "strict_replication",
            true
        },
        {
            "replica_maintenance_interval_ms",
            maintenance_interval_ms
        },
        {
            "peers",
            std::move(peer_payload)
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
            "Failed to create scheduler configuration."
        );
    }

    output
        << payload.dump(2)
        << '\n';

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write scheduler configuration."
        );
    }
}

void wait_for_server(
    nexusfs::http::HttpServer& server
)
{
    const auto deadline =
        std::chrono::steady_clock::now()
        + std::chrono::seconds{
            5
        };

    while (!server.is_running())
    {
        if (
            std::chrono::steady_clock::now()
            >= deadline
        )
        {
            throw std::runtime_error(
                "Scheduler peer server startup timed out."
            );
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds{
                5
            }
        );
    }
}

template <typename Predicate>
void wait_until(
    Predicate&& predicate,
    const std::string& error_message
)
{
    const auto deadline =
        std::chrono::steady_clock::now()
        + std::chrono::seconds{
            8
        };

    while (!predicate())
    {
        if (
            std::chrono::steady_clock::now()
            >= deadline
        )
        {
            throw std::runtime_error(
                error_message
            );
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds{
                10
            }
        );
    }
}

void stop_and_join(
    nexusfs::http::HttpServer& server,
    std::thread& thread
)
{
    server.stop();

    if (thread.joinable())
    {
        thread.join();
    }
}

void test_background_replica_recovery()
{
    TemporaryDirectory directory;

    const std::filesystem::path root_a =
        directory.path()
        / "node-a";

    const std::filesystem::path root_b =
        directory.path()
        / "node-b";

    const std::filesystem::path source_path =
        directory.path()
        / "scheduler-input.bin";

    const std::vector<std::uint8_t> source_data{
        2,
        4,
        6,
        8,
        10,
        12,
        14,
        16,
        18,
        20
    };

    write_binary_file(
        source_path,
        source_data
    );

    const nexusfs::app::NexusFsService
        local_service_a{
            root_a,
            1024
        };

    const auto stored =
        local_service_a.store_file(
            source_path
        );

    (void)stored;

    const auto chunks =
        nexusfs::storage::Chunker{
            1024
        }.split_file(
            source_path
        );

    require_equal(
        chunks.size(),
        static_cast<std::size_t>(1),
        "Scheduler single-chunk setup test"
    );

    const std::uint16_t port_a =
        reserve_port();

    const std::uint16_t port_b =
        reserve_port();

    const auto initial_a =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    root_a,
                    "127.0.0.1",
                    port_a
                );

    const auto initial_b =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    root_b,
                    "127.0.0.1",
                    port_b
                );

    const std::string cluster_id{
        "replica-scheduler-test"
    };

    write_configuration(
        initial_a->cluster_directory()
            / "cluster.json",
        cluster_id,
        port_a,
        2,
        100,
        {
            {
                initial_b->identity().node_id,
                "127.0.0.1",
                port_b
            }
        }
    );

    write_configuration(
        initial_b->cluster_directory()
            / "cluster.json",
        cluster_id,
        port_b,
        1,
        100,
        {
            {
                initial_a->identity().node_id,
                "127.0.0.1",
                port_a
            }
        }
    );

    const auto cluster_a =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    root_a,
                    "127.0.0.1",
                    port_a
                );

    const auto cluster_b =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    root_b,
                    "127.0.0.1",
                    port_b
                );

    require_equal(
        cluster_a
            ->configuration()
            .replica_maintenance_interval_ms,
        static_cast<std::uint64_t>(100),
        "Persisted maintenance interval test"
    );

    const auto metrics_a =
        std::make_shared<
            nexusfs::observability::
                MetricsRegistry
        >();

    std::ostringstream logs;

    const auto logger_a =
        std::make_shared<
            nexusfs::observability::
                JsonLogger
        >(
            &logs
        );

    const auto service_a =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            root_a,
            1024,
            cluster_a,
            2,
            true,
            metrics_a,
            logger_a
        );

    nexusfs::cluster::
        ReplicaMaintenanceScheduler scheduler{
            service_a,
            metrics_a,
            logger_a,
            std::chrono::milliseconds{
                cluster_a
                    ->configuration()
                    .replica_maintenance_interval_ms
            }
        };

    scheduler.start();

    wait_until(
        [
            &metrics_a
        ]()
        {
            const auto snapshot =
                metrics_a->snapshot();

            return (
                snapshot
                    .replica_maintenance_runs_total
                    >= 1
                && snapshot
                    .replica_maintenance_under_replicated_chunks
                    >= 1
            );
        },
        "Initial under-replication sweep timed out."
    );

    require_true(
        scheduler.is_running(),
        "Maintenance scheduler running-state test"
    );

    const auto service_b =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            root_b,
            1024
        );

    const auto metrics_b =
        std::make_shared<
            nexusfs::observability::
                MetricsRegistry
        >();

    const auto logger_b =
        std::make_shared<
            nexusfs::observability::
                JsonLogger
        >();

    const nexusfs::http::HttpRouter router_b{
        service_b,
        metrics_b,
        logger_b,
        cluster_b
    };

    nexusfs::http::HttpServer server_b{
        "127.0.0.1",
        port_b,
        router_b
    };

    std::exception_ptr server_exception;

    std::thread server_thread{
        [
            &server_b,
            &server_exception
        ]()
        {
            try
            {
                server_b.run();
            }
            catch (...)
            {
                server_exception =
                    std::current_exception();
            }
        }
    };

    wait_for_server(
        server_b
    );

    try
    {
        wait_until(
            [
                &metrics_a
            ]()
            {
                return (
                    metrics_a
                        ->snapshot()
                        .replica_maintenance_remote_replicas_created
                    >= 1
                );
            },
            "Background replacement replica creation timed out."
        );

        const nexusfs::storage::ChunkStore
            remote_store{
                root_b
            };

        require_true(
            remote_store.contains(
                chunks.front().hash
            ),
            "Background replica existence test"
        );

        require_equal(
            remote_store.load(
                chunks.front().hash
            ),
            chunks.front().data,
            "Background replica content test"
        );
    }
    catch (...)
    {
        scheduler.stop();

        stop_and_join(
            server_b,
            server_thread
        );

        throw;
    }

    scheduler.stop();
    scheduler.stop();

    stop_and_join(
        server_b,
        server_thread
    );

    if (server_exception)
    {
        std::rethrow_exception(
            server_exception
        );
    }

    require_true(
        !scheduler.is_running(),
        "Maintenance scheduler stopped-state test"
    );

    const auto snapshot =
        metrics_a->snapshot();

    require_equal(
        snapshot
            .replica_maintenance_scheduler_starts_total,
        static_cast<std::uint64_t>(1),
        "Maintenance scheduler start metric test"
    );

    require_equal(
        snapshot
            .replica_maintenance_scheduler_stops_total,
        static_cast<std::uint64_t>(1),
        "Maintenance scheduler stop metric test"
    );

    require_equal(
        snapshot
            .replica_maintenance_scheduler_failures_total,
        static_cast<std::uint64_t>(0),
        "Maintenance scheduler failure isolation test"
    );

    require_true(
        snapshot.replica_maintenance_runs_total >= 2,
        "Repeated maintenance sweep test"
    );

    require_true(
        snapshot
            .replica_maintenance_under_replicated_chunks
            >= 1,
        "Initial node-loss observation test"
    );

    require_true(
        snapshot
            .replica_maintenance_remote_replicas_created
            >= 1,
        "Recovered peer replica metric test"
    );

    const std::string log_output =
        logs.str();

    require_true(
        log_output.find(
            "replica_maintenance_scheduler_started"
        ) != std::string::npos,
        "Maintenance scheduler start-log test"
    );

    require_true(
        log_output.find(
            "replica_maintenance_scheduler_sweep_completed"
        ) != std::string::npos,
        "Maintenance scheduler sweep-log test"
    );

    require_true(
        log_output.find(
            "replica_maintenance_scheduler_stopped"
        ) != std::string::npos,
        "Maintenance scheduler stop-log test"
    );

    const nexusfs::http::HttpRouter
        metrics_router{
            service_a,
            metrics_a,
            logger_a,
            cluster_a
        };

    nexusfs::http::HttpRouter::Request
        metrics_request{
            beast_http::verb::get,
            "/api/v1/metrics",
            11
        };

    metrics_request.keep_alive(
        false
    );

    const auto response =
        metrics_router.route(
            metrics_request
        );

    require_equal(
        response.result(),
        beast_http::status::ok,
        "Maintenance scheduler metrics endpoint status test"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    require_equal(
        payload
            .at("cluster_transport")
            .at("maintenance")
            .at("scheduler")
            .at("starts")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Maintenance scheduler endpoint start test"
    );

    require_equal(
        payload
            .at("cluster_transport")
            .at("maintenance")
            .at("scheduler")
            .at("stops")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Maintenance scheduler endpoint stop test"
    );
}

}

int main()
{
    try
    {
        test_background_replica_recovery();

        std::cout
            << "[PASS] Background replica-maintenance recovery\n";

        std::cout
            << "All NexusFS replica-maintenance scheduler tests passed.\n";

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
