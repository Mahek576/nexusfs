#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/cluster/peer_transport.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
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
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{

namespace asio =
    boost::asio;

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
                "nexusfs-peer-transport-tests-"
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
    const std::string& cluster_id,
    const std::string& local_address,
    std::uint16_t local_port,
    const std::vector<
        nexusfs::cluster::PeerDefinition
    >& peers
)
{
    nlohmann::ordered_json peer_payload =
        nlohmann::ordered_json::array();

    for (
        const auto& peer :
        peers
    )
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
            local_address
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
            "peers",
            std::move(
                peer_payload
            )
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
            "Failed to create peer test configuration."
        );
    }

    output
        << payload.dump(2)
        << '\n';

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write peer test configuration."
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
                "Peer transport server startup timed out."
            );
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds{
                5
            }
        );
    }
}

void test_deterministic_replica_placement()
{
    const std::string hash(
        64,
        'a'
    );

    const std::vector<
        nexusfs::cluster::PeerDefinition
    > peers{
        {
            "11111111111111111111111111111111",
            "127.0.0.1",
            8501
        },
        {
            "22222222222222222222222222222222",
            "127.0.0.1",
            8502
        },
        {
            "33333333333333333333333333333333",
            "127.0.0.1",
            8503
        }
    };

    const auto first =
        nexusfs::cluster::
            ReplicationCoordinator::
                select_replica_peers(
                    hash,
                    peers,
                    2
                );

    const auto second =
        nexusfs::cluster::
            ReplicationCoordinator::
                select_replica_peers(
                    hash,
                    peers,
                    2
                );

    require_equal(
        first.size(),
        static_cast<std::size_t>(2),
        "Replica placement count test"
    );

    require_equal(
        first[0].node_id,
        second[0].node_id,
        "Replica placement first-peer test"
    );

    require_equal(
        first[1].node_id,
        second[1].node_id,
        "Replica placement second-peer test"
    );

    require_true(
        first[0].node_id !=
            first[1].node_id,
        "Replica placement uniqueness test"
    );
}

void test_real_peer_transport()
{
    TemporaryDirectory directory;

    const std::filesystem::path root_a =
        directory.path()
        / "node-a";

    const std::filesystem::path root_b =
        directory.path()
        / "node-b";

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

    const std::string node_a_id =
        initial_a->identity().node_id;

    const std::string node_b_id =
        initial_b->identity().node_id;

    const std::string cluster_id{
        "peer-transport-test"
    };

    write_configuration(
        initial_a->cluster_directory()
            / "cluster.json",
        cluster_id,
        "127.0.0.1",
        port_a,
        {
            {
                node_b_id,
                "127.0.0.1",
                port_b
            }
        }
    );

    write_configuration(
        initial_b->cluster_directory()
            / "cluster.json",
        cluster_id,
        "127.0.0.1",
        port_b,
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
        const auto& peer_b =
            cluster_a->
                configuration()
                .peers
                .front();

        nexusfs::cluster::PeerTransport
            transport{
                cluster_a,
                std::chrono::milliseconds{
                    3000
                }
            };

        transport.send_heartbeat(
            peer_b
        );

        const auto sender_health =
            cluster_a->peer_health();

        require_equal(
            sender_health.front().state,
            nexusfs::cluster::
                PeerHealthState::healthy,
            "Outbound heartbeat peer-state test"
        );

        const auto receiver_health =
            cluster_b->peer_health();

        require_equal(
            receiver_health.front().state,
            nexusfs::cluster::
                PeerHealthState::healthy,
            "Inbound heartbeat peer-state test"
        );

        const std::vector<std::uint8_t> data{
            0,
            1,
            2,
            3,
            4,
            5,
            0,
            255,
            128,
            64
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

        const auto first_result =
            transport.store_chunk(
                peer_b,
                hash,
                data
            );

        require_equal(
            first_result,
            nexusfs::cluster::
                RemoteChunkStoreResult::stored,
            "Remote chunk stored-result test"
        );

        const auto second_result =
            transport.store_chunk(
                peer_b,
                hash,
                data
            );

        require_equal(
            second_result,
            nexusfs::cluster::
                RemoteChunkStoreResult::
                    already_exists,
            "Remote chunk reuse-result test"
        );

        const nexusfs::storage::ChunkStore
            receiver_store{
                root_b
            };

        require_true(
            receiver_store.contains(
                hash
            ),
            "Remote receiver chunk-existence test"
        );

        require_equal(
            receiver_store.load(
                hash
            ),
            data,
            "Remote receiver chunk-content test"
        );

        require_equal(
            transport.load_chunk(
                peer_b,
                hash
            ),
            data,
            "Remote chunk download test"
        );

        nexusfs::cluster::
            ReplicationCoordinator coordinator{
                cluster_a,
                std::chrono::milliseconds{
                    3000
                }
            };

        const auto replication_report =
            coordinator.replicate_chunk(
                hash,
                data,
                2
            );

        require_equal(
            replication_report
                .requested_remote_replicas,
            static_cast<std::size_t>(1),
            "Replication requested-count test"
        );

        require_equal(
            replication_report
                .acknowledged_replicas,
            static_cast<std::size_t>(1),
            "Replication acknowledgement test"
        );

        require_true(
            replication_report.satisfied,
            "Replication satisfaction test"
        );
    }
    catch (...)
    {
        server_b.stop();
        server_thread.join();

        throw;
    }

    server_b.stop();
    server_thread.join();

    if (server_exception)
    {
        std::rethrow_exception(
            server_exception
        );
    }
}

}

int main()
{
    try
    {
        test_deterministic_replica_placement();

        std::cout
            << "[PASS] Deterministic replica placement\n";

        test_real_peer_transport();

        std::cout
            << "[PASS] Real peer heartbeat and chunk transport\n";

        std::cout
            << "All NexusFS peer transport tests passed.\n";

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
