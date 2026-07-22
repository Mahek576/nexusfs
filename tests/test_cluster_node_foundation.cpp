#include "nexusfs/cluster/cluster_node_foundation.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{

using nexusfs::cluster::
    ClusterNodeFoundation;

using nexusfs::cluster::
    HeartbeatMessage;

using nexusfs::cluster::
    PeerHealthState;

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
                "nexusfs-cluster-node-tests-"
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

template <typename Operation>
void require_exception(
    Operation&& operation,
    const std::string& test_name
)
{
    bool exception_thrown =
        false;

    try
    {
        operation();
    }
    catch (const std::exception&)
    {
        exception_thrown =
            true;
    }

    require_true(
        exception_thrown,
        test_name
    );
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
            "nexusfs-test-cluster"
        },
        {
            "advertise_address",
            "127.0.0.1"
        },
        {
            "advertise_port",
            8100
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
                        "11111111111111111111111111111111"
                    },
                    {
                        "address",
                        "127.0.0.1"
                    },
                    {
                        "port",
                        8101
                    }
                },
                {
                    {
                        "node_id",
                        "22222222222222222222222222222222"
                    },
                    {
                        "address",
                        "127.0.0.1"
                    },
                    {
                        "port",
                        8102
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
            "Failed to write cluster test configuration."
        );
    }

    output
        << payload.dump(2)
        << '\n';

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to close cluster test configuration."
        );
    }
}

void test_persistent_node_identity()
{
    TemporaryDirectory directory;

    const auto first =
        ClusterNodeFoundation::load_or_create(
            directory.path(),
            "127.0.0.1",
            8080
        );

    require_equal(
        first->identity().node_id.size(),
        static_cast<std::size_t>(32),
        "Persistent node-ID length test"
    );

    require_true(
        std::filesystem::exists(
            directory.path()
            / "cluster"
            / "node_identity.json"
        ),
        "Persistent identity-file test"
    );

    require_true(
        std::filesystem::exists(
            directory.path()
            / "cluster"
            / "cluster.json"
        ),
        "Persistent configuration-file test"
    );

    const std::string original_node_id =
        first->identity().node_id;

    const auto second =
        ClusterNodeFoundation::load_or_create(
            directory.path(),
            "127.0.0.1",
            8080
        );

    require_equal(
        second->identity().node_id,
        original_node_id,
        "Persistent identity reload test"
    );

    require_equal(
        second->configuration()
            .advertise_port,
        static_cast<std::uint16_t>(8080),
        "Default cluster port test"
    );
}

void test_peer_health_transitions()
{
    TemporaryDirectory directory;

    const auto initial =
        ClusterNodeFoundation::load_or_create(
            directory.path(),
            "127.0.0.1",
            8100
        );

    write_cluster_configuration(
        initial->cluster_directory()
        / "cluster.json"
    );

    const auto cluster =
        ClusterNodeFoundation::load_or_create(
            directory.path(),
            "127.0.0.1",
            8100
        );

    require_equal(
        cluster->configuration()
            .peers.size(),
        static_cast<std::size_t>(2),
        "Configured peer-count test"
    );

    const auto start =
        ClusterNodeFoundation::Clock::now();

    const HeartbeatMessage heartbeat{
        "nexusfs-test-cluster",
        "11111111111111111111111111111111",
        "127.0.0.1",
        8101,
        123456789
    };

    cluster->record_peer_heartbeat(
        heartbeat,
        start
    );

    auto peers =
        cluster->peer_health(
            start
        );

    require_equal(
        peers.front().state,
        PeerHealthState::healthy,
        "Healthy peer-state test"
    );

    peers =
        cluster->peer_health(
            start
            + std::chrono::milliseconds{
                3000
            }
        );

    require_equal(
        peers.front().state,
        PeerHealthState::suspect,
        "Suspect peer-state test"
    );

    peers =
        cluster->peer_health(
            start
            + std::chrono::milliseconds{
                6000
            }
        );

    require_equal(
        peers.front().state,
        PeerHealthState::unavailable,
        "Unavailable peer-state test"
    );

    cluster->record_peer_failure(
        "22222222222222222222222222222222",
        "connection refused"
    );

    const auto snapshot =
        cluster->snapshot(
            start
        );

    require_equal(
        snapshot.suspect_peers,
        static_cast<std::uint64_t>(1),
        "Peer-failure state test"
    );
}

void test_heartbeat_protocol_validation()
{
    TemporaryDirectory directory;

    const auto initial =
        ClusterNodeFoundation::load_or_create(
            directory.path(),
            "127.0.0.1",
            8100
        );

    write_cluster_configuration(
        initial->cluster_directory()
        / "cluster.json"
    );

    const auto cluster =
        ClusterNodeFoundation::load_or_create(
            directory.path(),
            "127.0.0.1",
            8100
        );

    const HeartbeatMessage heartbeat{
        "nexusfs-test-cluster",
        "11111111111111111111111111111111",
        "127.0.0.1",
        8101,
        123456789
    };

    const std::string encoded =
        ClusterNodeFoundation::
            encode_heartbeat(
                heartbeat
            );

    const HeartbeatMessage decoded =
        ClusterNodeFoundation::
            decode_heartbeat(
                encoded
            );

    require_equal(
        decoded.node_id,
        heartbeat.node_id,
        "Heartbeat round-trip node-ID test"
    );

    require_equal(
        decoded.advertise_port,
        heartbeat.advertise_port,
        "Heartbeat round-trip port test"
    );

    HeartbeatMessage wrong_cluster =
        heartbeat;

    wrong_cluster.cluster_id =
        "another-cluster";

    require_exception(
        [
            &cluster,
            &wrong_cluster
        ]()
        {
            cluster->record_peer_heartbeat(
                wrong_cluster
            );
        },
        "Cross-cluster heartbeat rejection test"
    );

    HeartbeatMessage wrong_endpoint =
        heartbeat;

    wrong_endpoint.advertise_port =
        9999;

    require_exception(
        [
            &cluster,
            &wrong_endpoint
        ]()
        {
            cluster->record_peer_heartbeat(
                wrong_endpoint
            );
        },
        "Heartbeat endpoint rejection test"
    );

    require_exception(
        []()
        {
            (void)ClusterNodeFoundation::
                decode_heartbeat(
                    "{invalid-json"
                );
        },
        "Malformed heartbeat rejection test"
    );
}

void test_concurrent_heartbeat_updates()
{
    constexpr std::size_t thread_count =
        8;

    constexpr std::size_t updates_per_thread =
        200;

    TemporaryDirectory directory;

    const auto initial =
        ClusterNodeFoundation::load_or_create(
            directory.path(),
            "127.0.0.1",
            8100
        );

    write_cluster_configuration(
        initial->cluster_directory()
        / "cluster.json"
    );

    const auto cluster =
        ClusterNodeFoundation::load_or_create(
            directory.path(),
            "127.0.0.1",
            8100
        );

    const HeartbeatMessage heartbeat{
        "nexusfs-test-cluster",
        "11111111111111111111111111111111",
        "127.0.0.1",
        8101,
        123456789
    };

    std::barrier start_barrier{
        static_cast<std::ptrdiff_t>(
            thread_count
        )
    };

    std::vector<std::thread> workers;

    workers.reserve(
        thread_count
    );

    for (
        std::size_t index = 0;
        index < thread_count;
        ++index
    )
    {
        workers.emplace_back(
            [
                &cluster,
                &heartbeat,
                &start_barrier
            ]()
            {
                start_barrier.arrive_and_wait();

                for (
                    std::size_t update = 0;
                    update < updates_per_thread;
                    ++update
                )
                {
                    cluster->record_peer_heartbeat(
                        heartbeat
                    );
                }
            }
        );
    }

    for (std::thread& worker : workers)
    {
        worker.join();
    }

    const auto peers =
        cluster->peer_health();

    require_equal(
        peers.front()
            .successful_heartbeats,
        static_cast<std::uint64_t>(
            thread_count
            * updates_per_thread
        ),
        "Concurrent heartbeat-count test"
    );

    require_equal(
        peers.front().state,
        PeerHealthState::healthy,
        "Concurrent heartbeat-state test"
    );
}

}

int main()
{
    try
    {
        test_persistent_node_identity();

        std::cout
            << "[PASS] Persistent cluster node identity\n";

        test_peer_health_transitions();

        std::cout
            << "[PASS] Cluster peer health transitions\n";

        test_heartbeat_protocol_validation();

        std::cout
            << "[PASS] Cluster heartbeat protocol\n";

        test_concurrent_heartbeat_updates();

        std::cout
            << "[PASS] Concurrent cluster heartbeats\n";

        std::cout
            << "All NexusFS cluster node foundation tests passed.\n";

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
