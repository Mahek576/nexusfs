#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/cluster/peer_transport.hpp"
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
                "nexusfs-replica-maintenance-tests-"
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
            "Failed to create maintenance test input."
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
            "Failed to write maintenance test input."
        );
    }
}

void write_configuration(
    const std::filesystem::path& path,
    const std::string& cluster_id,
    std::uint16_t local_port,
    std::size_t replication_factor,
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
            1000
        },
        {
            "failure_timeout_ms",
            5000
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
            "Failed to create maintenance configuration."
        );
    }

    output
        << payload.dump(2)
        << '\n';

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write maintenance configuration."
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
                "Maintenance server startup timed out."
            );
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds{
                5
            }
        );
    }
}

void stop_and_join(
    nexusfs::http::HttpServer& server,
    std::thread& server_thread
)
{
    server.stop();

    if (server_thread.joinable())
    {
        server_thread.join();
    }
}

void test_three_node_replacement_repair()
{
    TemporaryDirectory directory;

    const std::filesystem::path root_a =
        directory.path() / "node-a";

    const std::filesystem::path root_b =
        directory.path() / "node-b";

    const std::filesystem::path root_c =
        directory.path() / "node-c";

    const std::uint16_t port_a =
        reserve_port();

    const std::uint16_t port_b =
        reserve_port();

    const std::uint16_t port_c =
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

    const auto initial_c =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    root_c,
                    "127.0.0.1",
                    port_c
                );

    const std::string node_a_id =
        initial_a->identity().node_id;

    const std::string node_b_id =
        initial_b->identity().node_id;

    const std::string node_c_id =
        initial_c->identity().node_id;

    const std::string cluster_id{
        "replica-maintenance-test"
    };

    write_configuration(
        initial_a->cluster_directory()
            / "cluster.json",
        cluster_id,
        port_a,
        2,
        {
            {
                node_b_id,
                "127.0.0.1",
                port_b
            },
            {
                node_c_id,
                "127.0.0.1",
                port_c
            }
        }
    );

    write_configuration(
        initial_b->cluster_directory()
            / "cluster.json",
        cluster_id,
        port_b,
        1,
        {
            {
                node_a_id,
                "127.0.0.1",
                port_a
            }
        }
    );

    write_configuration(
        initial_c->cluster_directory()
            / "cluster.json",
        cluster_id,
        port_c,
        1,
        {
            {
                node_a_id,
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

    const auto cluster_c =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    root_c,
                    "127.0.0.1",
                    port_c
                );

    const auto service_b =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            root_b,
            1024
        );

    const auto service_c =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            root_c,
            1024
        );

    const auto metrics_b =
        std::make_shared<
            nexusfs::observability::
                MetricsRegistry
        >();

    const auto metrics_c =
        std::make_shared<
            nexusfs::observability::
                MetricsRegistry
        >();

    const auto logger_b =
        std::make_shared<
            nexusfs::observability::
                JsonLogger
        >();

    const auto logger_c =
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

    const nexusfs::http::HttpRouter router_c{
        service_c,
        metrics_c,
        logger_c,
        cluster_c
    };

    nexusfs::http::HttpServer server_b{
        "127.0.0.1",
        port_b,
        router_b
    };

    nexusfs::http::HttpServer server_c{
        "127.0.0.1",
        port_c,
        router_c
    };

    std::exception_ptr server_b_exception;
    std::exception_ptr server_c_exception;

    std::thread server_b_thread{
        [
            &server_b,
            &server_b_exception
        ]()
        {
            try
            {
                server_b.run();
            }
            catch (...)
            {
                server_b_exception =
                    std::current_exception();
            }
        }
    };

    std::thread server_c_thread{
        [
            &server_c,
            &server_c_exception
        ]()
        {
            try
            {
                server_c.run();
            }
            catch (...)
            {
                server_c_exception =
                    std::current_exception();
            }
        }
    };

    wait_for_server(
        server_b
    );

    wait_for_server(
        server_c
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

    const std::vector<std::uint8_t> source_data{
        1,
        3,
        5,
        7,
        9,
        11,
        13,
        15,
        17,
        19
    };

    const std::filesystem::path source_path =
        directory.path()
        / "maintenance-input.bin";

    write_binary_file(
        source_path,
        source_data
    );

    const auto chunks =
        nexusfs::storage::Chunker{
            1024
        }.split_file(
            source_path
        );

    require_equal(
        chunks.size(),
        static_cast<std::size_t>(1),
        "Maintenance single-chunk setup test"
    );

    const auto stored =
        service_a->store_file(
            source_path
        );

    (void)stored;

    const auto selected =
        nexusfs::cluster::
            ReplicationCoordinator::
                select_replica_peers(
                    chunks.front().hash,
                    cluster_a
                        ->configuration()
                        .peers,
                    1
                );

    require_equal(
        selected.size(),
        static_cast<std::size_t>(1),
        "Maintenance primary-selection test"
    );

    const bool primary_is_b =
        selected.front().node_id ==
        node_b_id;

    nexusfs::storage::ChunkStore store_b{
        root_b
    };

    nexusfs::storage::ChunkStore store_c{
        root_c
    };

    require_true(
        primary_is_b
            ? store_b.contains(
                  chunks.front().hash
              )
            : store_c.contains(
                  chunks.front().hash
              ),
        "Initial primary replica test"
    );

    require_true(
        primary_is_b
            ? !store_c.contains(
                  chunks.front().hash
              )
            : !store_b.contains(
                  chunks.front().hash
              ),
        "Initial replacement absence test"
    );

    try
    {
        if (primary_is_b)
        {
            stop_and_join(
                server_b,
                server_b_thread
            );
        }
        else
        {
            stop_and_join(
                server_c,
                server_c_thread
            );
        }

        const auto repaired =
            service_a->repair_replicas();

        require_equal(
            repaired.manifests_scanned,
            static_cast<std::size_t>(1),
            "Maintenance manifest-scan test"
        );

        require_equal(
            repaired.unique_chunks_scanned,
            static_cast<std::size_t>(1),
            "Maintenance chunk-scan test"
        );

        require_equal(
            repaired.remote_replicas_created,
            static_cast<std::size_t>(1),
            "Replacement replica creation test"
        );

        require_true(
            repaired.peer_failures >= 1,
            "Failed primary detection test"
        );

        require_equal(
            repaired.under_replicated_chunks,
            static_cast<std::size_t>(0),
            "Replacement repair policy test"
        );

        require_true(
            repaired.fully_repaired,
            "Replacement repair completion test"
        );

        require_true(
            primary_is_b
                ? store_c.contains(
                      chunks.front().hash
                  )
                : store_b.contains(
                      chunks.front().hash
                  ),
            "Replacement peer content test"
        );

        require_equal(
            primary_is_b
                ? store_c.load(
                      chunks.front().hash
                  )
                : store_b.load(
                      chunks.front().hash
                  ),
            chunks.front().data,
            "Replacement peer data test"
        );

        const auto successful_snapshot =
            metrics_a->snapshot();

        require_equal(
            successful_snapshot
                .replica_maintenance_runs_total,
            static_cast<std::uint64_t>(1),
            "Maintenance run metric test"
        );

        require_equal(
            successful_snapshot
                .replica_maintenance_remote_replicas_created,
            static_cast<std::uint64_t>(1),
            "Maintenance creation metric test"
        );

        require_true(
            logs.str().find(
                "replica_maintenance_completed"
            ) != std::string::npos,
            "Maintenance structured-log test"
        );

        if (primary_is_b)
        {
            stop_and_join(
                server_c,
                server_c_thread
            );
        }
        else
        {
            stop_and_join(
                server_b,
                server_b_thread
            );
        }

        const auto unavailable =
            service_a->repair_replicas();

        require_equal(
            unavailable.under_replicated_chunks,
            static_cast<std::size_t>(1),
            "Unavailable-cluster under-replication test"
        );

        require_true(
            !unavailable.fully_repaired,
            "Unavailable-cluster repair-status test"
        );

        require_true(
            unavailable.peer_failures >= 2,
            "Unavailable-cluster peer-failure test"
        );

        const auto final_snapshot =
            metrics_a->snapshot();

        require_equal(
            final_snapshot
                .replica_maintenance_runs_total,
            static_cast<std::uint64_t>(2),
            "Repeated maintenance run metric test"
        );

        require_true(
            final_snapshot
                .replica_maintenance_under_replicated_chunks
                >= 1,
            "Under-replication metric test"
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

        const auto metrics_response =
            metrics_router.route(
                metrics_request
            );

        const nlohmann::json payload =
            nlohmann::json::parse(
                metrics_response.body()
            );

        require_equal(
            payload
                .at("cluster_transport")
                .at("maintenance")
                .at("runs")
                .get<std::uint64_t>(),
            static_cast<std::uint64_t>(2),
            "Maintenance metrics endpoint test"
        );
    }
    catch (...)
    {
        stop_and_join(
            server_b,
            server_b_thread
        );

        stop_and_join(
            server_c,
            server_c_thread
        );

        throw;
    }

    stop_and_join(
        server_b,
        server_b_thread
    );

    stop_and_join(
        server_c,
        server_c_thread
    );

    if (server_b_exception)
    {
        std::rethrow_exception(
            server_b_exception
        );
    }

    if (server_c_exception)
    {
        std::rethrow_exception(
            server_c_exception
        );
    }
}

}

int main()
{
    try
    {
        test_three_node_replacement_repair();

        std::cout
            << "[PASS] Three-node replacement replica repair\n";

        std::cout
            << "All NexusFS replica-maintenance tests passed.\n";

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
