#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
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
                "nexusfs-replica-repair-tests-"
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
            "Failed to create replica-repair test file."
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
            "Failed to write replica-repair test file."
        );
    }
}

std::vector<std::uint8_t>
read_binary_file(
    const std::filesystem::path& path
)
{
    std::ifstream input{
        path,
        std::ios::binary
    };

    if (!input.is_open())
    {
        throw std::runtime_error(
            "Failed to open repaired output file."
        );
    }

    return std::vector<std::uint8_t>{
        std::istreambuf_iterator<char>{
            input
        },
        std::istreambuf_iterator<char>{}
    };
}

std::filesystem::path chunk_path(
    const std::filesystem::path& root,
    const std::string& hash
)
{
    return (
        root
        / "chunks"
        / hash.substr(
            0,
            2
        )
        / hash.substr(
            2
        )
    );
}

void remove_local_chunk(
    const std::filesystem::path& root,
    const std::string& hash
)
{
    std::error_code removal_error;

    const bool removed =
        std::filesystem::remove(
            chunk_path(
                root,
                hash
            ),
            removal_error
        );

    if (
        removal_error
        || !removed
    )
    {
        throw std::runtime_error(
            "Failed to remove local test chunk."
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
            "Failed to create replica-repair configuration."
        );
    }

    output
        << payload.dump(2)
        << '\n';

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write replica-repair configuration."
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
                "Replica-repair server startup timed out."
            );
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds{
                5
            }
        );
    }
}

void test_remote_restore_and_verify_repair()
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

    const std::string cluster_id{
        "replica-repair-test"
    };

    write_configuration(
        initial_a->cluster_directory()
            / "cluster.json",
        cluster_id,
        port_a,
        2,
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

    const auto service_b =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            root_b,
            512
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
            512,
            cluster_a,
            2,
            true,
            metrics_a,
            logger_a
        );

    std::vector<std::uint8_t> source_data(
        2500
    );

    for (
        std::size_t index = 0;
        index < source_data.size();
        ++index
    )
    {
        source_data[index] =
            static_cast<std::uint8_t>(
                (
                    index * 37
                    + index / 512
                )
                % 251
            );
    }

    const std::filesystem::path source_path =
        directory.path()
        / "repair-source.bin";

    write_binary_file(
        source_path,
        source_data
    );

    const auto chunks =
        nexusfs::storage::Chunker{
            512
        }.split_file(
            source_path
        );

    require_true(
        chunks.size() >= 3,
        "Replica-repair chunk-count test"
    );

    const auto stored =
        service_a->store_file(
            source_path
        );

    try
    {
        remove_local_chunk(
            root_a,
            chunks[0].hash
        );

        const std::filesystem::path restore_path =
            directory.path()
            / "restored.bin";

        const auto restored =
            service_a->restore_file(
                stored.manifest_id,
                restore_path
            );

        require_equal(
            restored.bytes_written,
            static_cast<std::uint64_t>(
                source_data.size()
            ),
            "Remote fallback restoration-size test"
        );

        require_equal(
            read_binary_file(
                restore_path
            ),
            source_data,
            "Remote fallback restoration-content test"
        );

        const nexusfs::storage::ChunkStore
            local_store{
                root_a
            };

        require_true(
            local_store.contains(
                chunks[0].hash
            ),
            "Restoration local self-healing test"
        );

        require_equal(
            local_store.load(
                chunks[0].hash
            ),
            chunks[0].data,
            "Restoration repaired-chunk content test"
        );

        remove_local_chunk(
            root_a,
            chunks[1].hash
        );

        const auto verified =
            service_a->verify_file(
                stored.manifest_id
            );

        require_equal(
            verified.total_bytes_verified,
            static_cast<std::uint64_t>(
                source_data.size()
            ),
            "Remote fallback verification test"
        );

        require_true(
            local_store.contains(
                chunks[1].hash
            ),
            "Verification local self-healing test"
        );

        const auto recovery_snapshot =
            metrics_a->snapshot();

        require_equal(
            recovery_snapshot
                .remote_chunk_reads_succeeded,
            static_cast<std::uint64_t>(2),
            "Remote recovery success metric test"
        );

        require_equal(
            recovery_snapshot
                .local_chunk_repairs_total,
            static_cast<std::uint64_t>(2),
            "Local chunk repair metric test"
        );

        require_true(
            logs.str().find(
                "chunk_recovery_succeeded"
            ) != std::string::npos,
            "Chunk recovery success-log test"
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

        const nlohmann::json metrics_payload =
            nlohmann::json::parse(
                metrics_response.body()
            );

        require_equal(
            metrics_payload
                .at("cluster_transport")
                .at("recovery")
                .at("local_repairs")
                .get<std::uint64_t>(),
            static_cast<std::uint64_t>(2),
            "Recovery metrics endpoint test"
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

    remove_local_chunk(
        root_a,
        chunks[2].hash
    );

    const std::filesystem::path failed_output =
        directory.path()
        / "unavailable-peer-output.bin";

    require_exception(
        [
            &service_a,
            &stored,
            &failed_output
        ]()
        {
            (void)service_a->restore_file(
                stored.manifest_id,
                failed_output
            );
        },
        "Unavailable-peer recovery failure test"
    );

    require_true(
        !std::filesystem::exists(
            failed_output
        ),
        "Failed recovery output-protection test"
    );

    const auto failed_snapshot =
        metrics_a->snapshot();

    require_true(
        failed_snapshot
            .remote_chunk_reads_failed >= 1,
        "Remote recovery failure metric test"
    );

    require_true(
        logs.str().find(
            "chunk_recovery_failed"
        ) != std::string::npos,
        "Chunk recovery failure-log test"
    );
}

}

int main()
{
    try
    {
        test_remote_restore_and_verify_repair();

        std::cout
            << "[PASS] Remote restore and verification repair\n";

        std::cout
            << "All NexusFS replica-repair tests passed.\n";

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
