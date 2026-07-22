#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/cluster/metadata_catalog.hpp"
#include "nexusfs/cluster/metadata_catalog_synchronizer.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
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
                "nexusfs-catalog-sync-tests-"
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
                    index * 29
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
            "Failed to create catalog synchronization file."
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
            "Failed to write catalog synchronization file."
        );
    }
}

void write_configuration(
    const std::filesystem::path& path,
    const std::string& cluster_id,
    std::uint16_t local_port,
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
            1
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
            "Failed to create catalog synchronization configuration."
        );
    }

    output
        << payload.dump(2)
        << '\n';

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write catalog synchronization configuration."
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
                "Catalog synchronization server startup timed out."
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

void test_conflict_detection()
{
    const std::string manifest_id(
        64,
        'a'
    );

    const auto first =
        nexusfs::cluster::
            MetadataCatalogCodec::create(
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                {
                    {
                        manifest_id,
                        "first.bin",
                        512,
                        512,
                        1
                    }
                }
            );

    const auto second =
        nexusfs::cluster::
            MetadataCatalogCodec::create(
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                {
                    {
                        manifest_id,
                        "conflicting.bin",
                        512,
                        512,
                        1
                    }
                }
            );

    const auto merged =
        nexusfs::cluster::
            MetadataCatalogSynchronizer::
                merge_snapshots(
                    {
                        second,
                        first
                    }
                );

    require_equal(
        merged.entries.size(),
        static_cast<std::size_t>(1),
        "Deterministic conflict merge size test"
    );

    require_equal(
        merged.conflicts.size(),
        static_cast<std::size_t>(1),
        "Metadata catalog conflict detection test"
    );

    require_equal(
        merged.entries.front().original_filename,
        std::string{
            "first.bin"
        },
        "Deterministic conflict winner test"
    );
}

void test_three_node_catalog_convergence()
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

    const std::string cluster_id{
        "catalog-sync-test"
    };

    write_configuration(
        initial_a->cluster_directory()
            / "cluster.json",
        cluster_id,
        port_a,
        {
            {
                initial_b->identity().node_id,
                "127.0.0.1",
                port_b
            },
            {
                initial_c->identity().node_id,
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
        {
            {
                initial_a->identity().node_id,
                "127.0.0.1",
                port_a
            },
            {
                initial_c->identity().node_id,
                "127.0.0.1",
                port_c
            }
        }
    );

    write_configuration(
        initial_c->cluster_directory()
            / "cluster.json",
        cluster_id,
        port_c,
        {
            {
                initial_a->identity().node_id,
                "127.0.0.1",
                port_a
            },
            {
                initial_b->identity().node_id,
                "127.0.0.1",
                port_b
            }
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
            512
        );

    const auto service_c =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            root_c,
            512
        );

    const std::filesystem::path file_b =
        directory.path() / "node-b-file.bin";

    const std::filesystem::path file_c =
        directory.path() / "node-c-file.bin";

    write_binary_file(
        file_b,
        900,
        17
    );

    write_binary_file(
        file_c,
        1300,
        43
    );

    const auto stored_b =
        service_b->store_file(
            file_b
        );

    const auto stored_c =
        service_c->store_file(
            file_c
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

    const auto service_a =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            root_a,
            512,
            cluster_a,
            1,
            true,
            metrics_a,
            logger_a
        );

    const nexusfs::http::HttpRouter router_a{
        service_a,
        metrics_a,
        logger_a,
        cluster_a
    };

    try
    {
        const auto first_sync =
            service_a->
                synchronize_metadata_catalog();

        require_equal(
            first_sync.peers_contacted,
            static_cast<std::size_t>(2),
            "Catalog peer-contact count test"
        );

        require_equal(
            first_sync.peers_succeeded,
            static_cast<std::size_t>(2),
            "Catalog peer-success count test"
        );

        require_equal(
            first_sync.manifests_recovered,
            static_cast<std::size_t>(2),
            "Catalog manifest recovery count test"
        );

        require_equal(
            first_sync.conflicts_detected,
            static_cast<std::size_t>(0),
            "Catalog convergence conflict test"
        );

        require_true(
            first_sync.converged,
            "Three-node catalog convergence test"
        );

        require_equal(
            first_sync.files.size(),
            static_cast<std::size_t>(2),
            "Cluster-wide catalog summary test"
        );

        const nexusfs::storage::ManifestStore
            local_manifest_store{
                root_a
            };

        require_true(
            local_manifest_store.contains(
                stored_b.manifest_id
            ),
            "Node B manifest synchronization test"
        );

        require_true(
            local_manifest_store.contains(
                stored_c.manifest_id
            ),
            "Node C manifest synchronization test"
        );

        const auto second_sync =
            service_a->
                synchronize_metadata_catalog();

        require_equal(
            second_sync.manifests_recovered,
            static_cast<std::size_t>(0),
            "Catalog synchronization idempotency test"
        );

        require_equal(
            second_sync.manifests_already_local,
            static_cast<std::size_t>(2),
            "Catalog local reuse test"
        );

        nexusfs::http::HttpRouter::Request
            sync_request{
                beast_http::verb::post,
                "/api/v1/cluster/catalog/sync",
                11
            };

        sync_request.set(
            "X-NexusFS-Cluster-ID",
            cluster_id
        );

        sync_request.set(
            "X-NexusFS-Node-ID",
            cluster_b->identity().node_id
        );

        sync_request.keep_alive(
            false
        );

        const auto sync_response =
            router_a.route(
                sync_request
            );

        require_equal(
            sync_response.result(),
            beast_http::status::ok,
            "Catalog synchronization HTTP status test"
        );

        const nlohmann::json sync_payload =
            nlohmann::json::parse(
                sync_response.body()
            );

        require_true(
            sync_payload
                .at("converged")
                .get<bool>(),
            "Catalog synchronization HTTP convergence test"
        );

        require_equal(
            sync_payload
                .at("files")
                .size(),
            static_cast<std::size_t>(2),
            "Catalog synchronization HTTP summary test"
        );

        stop_and_join(
            server_c,
            server_c_thread
        );

        const auto partial_sync =
            service_a->
                synchronize_metadata_catalog();

        require_equal(
            partial_sync.peers_failed,
            static_cast<std::size_t>(1),
            "Partial catalog peer-failure test"
        );

        require_true(
            !partial_sync.converged,
            "Partial catalog convergence-status test"
        );

        require_true(
            local_manifest_store.contains(
                stored_b.manifest_id
            ),
            "Partial sync local metadata preservation test B"
        );

        require_true(
            local_manifest_store.contains(
                stored_c.manifest_id
            ),
            "Partial sync local metadata preservation test C"
        );

        const auto snapshot =
            metrics_a->snapshot();

        require_true(
            snapshot
                .metadata_catalog_sync_runs_total
                >= 4,
            "Catalog synchronization run metric test"
        );

        require_true(
            snapshot
                .metadata_catalog_sync_converged_total
                >= 3,
            "Catalog synchronization convergence metric test"
        );

        require_true(
            snapshot
                .metadata_catalog_sync_incomplete_total
                >= 1,
            "Catalog synchronization incomplete metric test"
        );

        require_equal(
            snapshot
                .metadata_catalog_manifests_recovered,
            static_cast<std::uint64_t>(2),
            "Catalog recovery metric test"
        );

        require_true(
            logs.str().find(
                "metadata_catalog_sync_completed"
            ) != std::string::npos,
            "Catalog synchronization structured-log test"
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

        const nlohmann::json metrics_payload =
            nlohmann::json::parse(
                metrics_response.body()
            );

        require_true(
            metrics_payload
                .at("cluster_transport")
                .at("metadata")
                .at("catalog_synchronization")
                .at("runs")
                .get<std::uint64_t>()
                >= 4,
            "Catalog synchronization endpoint metric test"
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
        test_conflict_detection();

        std::cout
            << "[PASS] Deterministic catalog conflict protection\n";

        test_three_node_catalog_convergence();

        std::cout
            << "[PASS] Three-node metadata catalog convergence\n";

        std::cout
            << "[PASS] Additive stale-view protection\n";

        std::cout
            << "[PASS] Catalog synchronization metrics and HTTP control\n";

        std::cout
            << "All NexusFS metadata catalog synchronization tests passed.\n";

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
