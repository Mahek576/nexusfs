#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/cluster/operation_journal.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/manifest_store.hpp"

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
                "nexusfs-consistency-rebalancing-"
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
    std::size_t size,
    std::uint8_t seed
)
{
    std::vector<std::uint8_t> data(
        size
    );

    for (
        std::size_t index = 0;
        index < data.size();
        ++index
    )
    {
        data[index] =
            static_cast<std::uint8_t>(
                (
                    index * 37
                    + seed
                )
                % 251
            );
    }

    std::ofstream output{
        path,
        std::ios::binary
            | std::ios::trunc
    };

    if (!output.is_open())
    {
        throw std::runtime_error(
            "Failed to create rebalance test file."
        );
    }

    output.write(
        reinterpret_cast<const char*>(
            data.data()
        ),
        static_cast<std::streamsize>(
            data.size()
        )
    );

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write rebalance test file."
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
            "replica_maintenance_interval_ms",
            30000
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
            "Failed to create cluster configuration."
        );
    }

    output
        << payload.dump(2)
        << '\n';

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write cluster configuration."
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
                "Rebalance peer server startup timed out."
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
    std::thread& thread
)
{
    server.stop();

    if (thread.joinable())
    {
        thread.join();
    }
}

std::vector<std::string> manifest_chunks(
    const std::filesystem::path& root,
    const std::string& manifest_id
)
{
    const nexusfs::storage::ManifestStore
        manifest_store{
            root
        };

    const auto manifest =
        nexusfs::storage::
            FileManifestCodec::decode(
                manifest_store.load(
                    manifest_id
                )
            );

    return manifest.chunk_hashes();
}

nexusfs::http::HttpRouter::Request
rebalance_request(
    const std::string& cluster_id,
    const std::string& node_id,
    const std::string& operation_id,
    std::uint64_t expected_epoch
)
{
    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::post,
        "/api/v1/cluster/rebalance",
        11
    };

    request.set(
        "X-NexusFS-Cluster-ID",
        cluster_id
    );

    request.set(
        "X-NexusFS-Node-ID",
        node_id
    );

    request.set(
        beast_http::field::content_type,
        "application/json"
    );

    request.keep_alive(
        false
    );

    request.body() =
        nlohmann::ordered_json{
            {
                "operation_id",
                operation_id
            },
            {
                "expected_membership_epoch",
                expected_epoch
            }
        }.dump();

    request.prepare_payload();

    return request;
}

void test_consistency_and_rebalancing()
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
            ClusterNodeFoundation::load_or_create(
                root_a,
                "127.0.0.1",
                port_a
            );

    const auto initial_b =
        nexusfs::cluster::
            ClusterNodeFoundation::load_or_create(
                root_b,
                "127.0.0.1",
                port_b
            );

    const auto initial_c =
        nexusfs::cluster::
            ClusterNodeFoundation::load_or_create(
                root_c,
                "127.0.0.1",
                port_c
            );

    const nexusfs::cluster::PeerDefinition peer_a{
        initial_a->identity().node_id,
        "127.0.0.1",
        port_a
    };

    const nexusfs::cluster::PeerDefinition peer_b{
        initial_b->identity().node_id,
        "127.0.0.1",
        port_b
    };

    const nexusfs::cluster::PeerDefinition peer_c{
        initial_c->identity().node_id,
        "127.0.0.1",
        port_c
    };

    const std::string cluster_id{
        "consistency-rebalancing-test"
    };

    /*
     * Node A intentionally starts with only B. Node C is joined
     * dynamically after the first incomplete rebalance.
     */
    write_configuration(
        initial_a->cluster_directory()
            / "cluster.json",
        cluster_id,
        port_a,
        3,
        {
            peer_b
        }
    );

    write_configuration(
        initial_b->cluster_directory()
            / "cluster.json",
        cluster_id,
        port_b,
        3,
        {
            peer_a,
            peer_c
        }
    );

    write_configuration(
        initial_c->cluster_directory()
            / "cluster.json",
        cluster_id,
        port_c,
        3,
        {
            peer_a,
            peer_b
        }
    );

    const auto cluster_a =
        nexusfs::cluster::
            ClusterNodeFoundation::load_or_create(
                root_a,
                "127.0.0.1",
                port_a
            );

    const auto cluster_b =
        nexusfs::cluster::
            ClusterNodeFoundation::load_or_create(
                root_b,
                "127.0.0.1",
                port_b
            );

    const auto cluster_c =
        nexusfs::cluster::
            ClusterNodeFoundation::load_or_create(
                root_c,
                "127.0.0.1",
                port_c
            );

    const auto service_b =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            root_b,
            256
        );

    const auto service_c =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            root_c,
            256
        );

    const auto metrics_b =
        std::make_shared<
            nexusfs::observability::MetricsRegistry
        >();

    const auto metrics_c =
        std::make_shared<
            nexusfs::observability::MetricsRegistry
        >();

    const auto logger_b =
        std::make_shared<
            nexusfs::observability::JsonLogger
        >();

    const auto logger_c =
        std::make_shared<
            nexusfs::observability::JsonLogger
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
        [&]()
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
        [&]()
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

    wait_for_server(server_b);
    wait_for_server(server_c);

    const auto metrics_a =
        std::make_shared<
            nexusfs::observability::MetricsRegistry
        >();

    std::ostringstream logs;

    const auto logger_a =
        std::make_shared<
            nexusfs::observability::JsonLogger
        >(
            &logs
        );

    const auto local_service_a =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            root_a,
            256
        );

    const auto cluster_service_a =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            root_a,
            256,
            cluster_a,
            3,
            true,
            metrics_a,
            logger_a
        );

    const nexusfs::http::HttpRouter router_a{
        cluster_service_a,
        metrics_a,
        logger_a,
        cluster_a
    };

    const std::filesystem::path first_file =
        directory.path()
        / "first-file.bin";

    write_binary_file(
        first_file,
        1536,
        19
    );

    const auto first_stored =
        local_service_a->store_file(
            first_file
        );

    const std::vector<std::string>
        first_chunks =
            manifest_chunks(
                root_a,
                first_stored.manifest_id
            );

    require_true(
        !first_chunks.empty(),
        "Rebalance source chunk test"
    );

    try
    {
        const std::uint64_t initial_epoch =
            cluster_a->membership_epoch();

        require_equal(
            initial_epoch,
            static_cast<std::uint64_t>(1),
            "Initial rebalance membership epoch test"
        );

        const auto before_join =
            cluster_service_a->rebalance_cluster(
                "rebalance-before-join",
                initial_epoch
            );

        require_true(
            !before_join.converged,
            "Pre-join under-replication test"
        );

        require_equal(
            before_join.under_replicated_chunks,
            before_join.unique_chunks_scanned,
            "Pre-join placement deficit test"
        );

        const nexusfs::storage::ChunkStore
            chunk_store_b{
                root_b
            };

        for (
            const std::string& chunk_hash :
            first_chunks
        )
        {
            require_true(
                chunk_store_b.contains(
                    chunk_hash
                ),
                "Pre-join node B placement test"
            );
        }

        const auto join_result =
            cluster_a->upsert_peer(
                peer_c,
                initial_epoch
            );

        require_true(
            join_result.applied,
            "Dynamic join before rebalance test"
        );

        require_equal(
            join_result.epoch,
            static_cast<std::uint64_t>(2),
            "Post-join membership epoch test"
        );

        const auto after_join =
            cluster_service_a->rebalance_cluster(
                "rebalance-after-join",
                join_result.epoch
            );

        require_true(
            after_join.converged,
            "Post-join placement convergence test"
        );

        require_equal(
            after_join.under_replicated_chunks,
            static_cast<std::uint64_t>(0),
            "Post-join under-replication test"
        );

        require_true(
            after_join.replicas_created > 0,
            "Post-join replica creation test"
        );

        const nexusfs::storage::ChunkStore
            chunk_store_c{
                root_c
            };

        for (
            const std::string& chunk_hash :
            first_chunks
        )
        {
            require_true(
                chunk_store_c.contains(
                    chunk_hash
                ),
                "Post-join node C placement test"
            );
        }

        /*
         * A fresh service instance must replay the durable result
         * without issuing another placement operation.
         */
        const auto restarted_service_a =
            std::make_shared<
                nexusfs::app::NexusFsService
            >(
                root_a,
                256,
                cluster_a,
                3,
                true,
                metrics_a,
                logger_a
            );

        const auto restarted_replay =
            restarted_service_a->
                rebalance_cluster(
                    "rebalance-after-join",
                    join_result.epoch
                );

        require_true(
            restarted_replay.replayed,
            "Restart-safe rebalance replay test"
        );

        require_equal(
            restarted_replay.request_digest,
            after_join.request_digest,
            "Durable rebalance digest replay test"
        );

        const std::filesystem::path second_file =
            directory.path()
            / "second-file.bin";

        write_binary_file(
            second_file,
            900,
            73
        );

        (void)local_service_a->store_file(
            second_file
        );

        bool operation_conflict =
            false;

        try
        {
            (void)cluster_service_a->
                rebalance_cluster(
                    "rebalance-after-join",
                    join_result.epoch
                );
        }
        catch (
            const nexusfs::cluster::
                OperationIdConflict&
        )
        {
            operation_conflict =
                true;
        }

        require_true(
            operation_conflict,
            "Operation ID payload-conflict test"
        );

        const auto stale =
            cluster_service_a->rebalance_cluster(
                "rebalance-stale-epoch",
                initial_epoch
            );

        require_equal(
            stale.status,
            std::string{
                "stale_membership_epoch"
            },
            "Stale rebalance epoch rejection test"
        );

        require_true(
            !stale.applied,
            "Stale rebalance side-effect barrier test"
        );

        const auto leave_result =
            cluster_a->remove_peer(
                peer_b.node_id,
                join_result.epoch
            );

        require_true(
            leave_result.applied,
            "Dynamic leave before rebalance test"
        );

        require_equal(
            leave_result.epoch,
            static_cast<std::uint64_t>(3),
            "Post-leave membership epoch test"
        );

        const auto after_leave =
            cluster_service_a->rebalance_cluster(
                "rebalance-after-leave",
                leave_result.epoch
            );

        require_true(
            !after_leave.converged,
            "Post-leave placement deficit test"
        );

        require_true(
            after_leave.under_replicated_chunks > 0,
            "Post-leave under-replication count test"
        );

        /*
         * Rebalancing is intentionally additive. Removing B from
         * membership must not delete previously safe data from B.
         */
        for (
            const std::string& chunk_hash :
            first_chunks
        )
        {
            require_true(
                chunk_store_b.contains(
                    chunk_hash
                ),
                "Additive rebalance stale-view protection test"
            );
        }

        const auto http_replay_response =
            router_a.route(
                rebalance_request(
                    cluster_id,
                    cluster_a
                        ->identity()
                        .node_id,
                    "rebalance-after-leave",
                    leave_result.epoch
                )
            );

        require_equal(
            http_replay_response.result(),
            beast_http::status::multi_status,
            "HTTP incomplete replay status test"
        );

        const nlohmann::json http_replay_payload =
            nlohmann::json::parse(
                http_replay_response.body()
            );

        require_true(
            http_replay_payload.at(
                "replayed"
            ).get<bool>(),
            "HTTP idempotent replay test"
        );

        const auto second_http_replay =
            router_a.route(
                rebalance_request(
                    cluster_id,
                    cluster_a
                        ->identity()
                        .node_id,
                    "rebalance-after-leave",
                    leave_result.epoch
                )
            );

        require_equal(
            second_http_replay.result(),
            beast_http::status::multi_status,
            "Repeated HTTP rebalance replay test"
        );

        const auto stale_http_response =
            router_a.route(
                rebalance_request(
                    cluster_id,
                    cluster_a
                        ->identity()
                        .node_id,
                    "http-stale-rebalance",
                    join_result.epoch
                )
            );

        require_equal(
            stale_http_response.result(),
            beast_http::status::conflict,
            "HTTP membership fencing test"
        );

        require_equal(
            router_a.normalized_route(
                rebalance_request(
                    cluster_id,
                    cluster_a
                        ->identity()
                        .node_id,
                    "route-normalization",
                    leave_result.epoch
                )
            ),
            std::string_view{
                "/api/v1/cluster/rebalance"
            },
            "Rebalance route normalization test"
        );

        const auto metric_snapshot =
            metrics_a->snapshot();

        require_true(
            metric_snapshot
                .rebalancing_runs_total
                >= 8,
            "Rebalance run metric test"
        );

        require_true(
            metric_snapshot
                .rebalancing_completed_total
                >= 3,
            "Rebalance completion metric test"
        );

        require_true(
            metric_snapshot
                .rebalancing_replayed_total
                >= 3,
            "Rebalance replay metric test"
        );

        require_true(
            metric_snapshot
                .rebalancing_stale_epoch_total
                >= 2,
            "Rebalance stale-epoch metric test"
        );

        require_true(
            metric_snapshot
                .rebalancing_idempotency_conflicts_total
                >= 1,
            "Rebalance idempotency-conflict metric test"
        );

        require_true(
            metric_snapshot
                .rebalancing_replicas_created
                > 0,
            "Rebalance replica-creation metric test"
        );

        require_true(
            logs.str().find(
                "cluster_rebalance_completed"
            ) != std::string::npos,
            "Rebalance completion structured-log test"
        );

        require_true(
            logs.str().find(
                "cluster_rebalance_replayed"
            ) != std::string::npos,
            "Rebalance replay structured-log test"
        );

        require_true(
            logs.str().find(
                "cluster_rebalance_stale_epoch"
            ) != std::string::npos,
            "Rebalance fencing structured-log test"
        );

        require_true(
            logs.str().find(
                "cluster_rebalance_idempotency_conflict"
            ) != std::string::npos,
            "Rebalance conflict structured-log test"
        );

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
            router_a.route(
                metrics_request
            );

        require_equal(
            metrics_response.result(),
            beast_http::status::ok,
            "Rebalance metrics endpoint status test"
        );

        const nlohmann::json metrics_payload =
            nlohmann::json::parse(
                metrics_response.body()
            );

        require_true(
            metrics_payload
                .at("cluster_transport")
                .at("rebalancing")
                .at("runs")
                .get<std::uint64_t>()
                >= 8,
            "Rebalance metrics endpoint test"
        );

        const std::filesystem::path journal_directory =
            cluster_a->cluster_directory()
            / "operations"
            / "rebalance";

        std::size_t journal_records =
            0;

        for (
            const std::filesystem::directory_entry& entry :
            std::filesystem::directory_iterator{
                journal_directory
            }
        )
        {
            if (
                entry.is_regular_file()
                && entry.path()
                    .extension() ==
                    ".json"
            )
            {
                ++journal_records;
            }
        }

        require_equal(
            journal_records,
            static_cast<std::size_t>(3),
            "Durable completed-operation record test"
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
        test_consistency_and_rebalancing();

        std::cout
            << "[PASS] Membership-epoch operation fencing\n";

        std::cout
            << "[PASS] Durable idempotent result replay\n";

        std::cout
            << "[PASS] Operation-ID conflict protection\n";

        std::cout
            << "[PASS] Join-aware deterministic rebalancing\n";

        std::cout
            << "[PASS] Leave-aware additive safety\n";

        std::cout
            << "[PASS] Rebalancing HTTP control and observability\n";

        std::cout
            << "All NexusFS consistency and rebalancing tests passed.\n";

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
