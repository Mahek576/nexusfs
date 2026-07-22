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
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
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
                "nexusfs-automatic-replication-tests-"
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
            "Failed to create replication test file."
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
            "Failed to write replication test file."
        );
    }
}

void write_configuration(
    const std::filesystem::path& path,
    const std::string& cluster_id,
    const std::string& address,
    std::uint16_t port,
    std::size_t replication_factor,
    bool strict_replication,
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
            address
        },
        {
            "advertise_port",
            port
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
            strict_replication
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
                "Replication server startup timed out."
            );
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds{
                5
            }
        );
    }
}

void test_automatic_two_node_replication()
{
    TemporaryDirectory directory;

    const std::filesystem::path root_a =
        directory.path() / "node-a";

    const std::filesystem::path root_b =
        directory.path() / "node-b";

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
        "automatic-replication-test"
    };

    write_configuration(
        initial_a->cluster_directory()
            / "cluster.json",
        cluster_id,
        "127.0.0.1",
        port_a,
        2,
        true,
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
        1,
        true,
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

    try
    {
        const auto service_a =
            std::make_shared<
                nexusfs::app::NexusFsService
            >(
                root_a,
                512,
                cluster_a,
                2,
                true
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
                    index % 251
                );
        }

        const std::filesystem::path source_path =
            directory.path()
            / "replicated-input.bin";

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

        const auto result =
            service_a->store_file(
                source_path
            );

        require_equal(
            result.replication_factor,
            static_cast<std::size_t>(2),
            "Automatic replication factor test"
        );

        require_equal(
            result.remote_replica_acknowledgements,
            chunks.size(),
            "Automatic replication acknowledgement test"
        );

        require_true(
            result.replication_satisfied,
            "Automatic replication satisfaction test"
        );

        require_true(
            result.manifest_stored,
            "Replicated manifest publication test"
        );

        const nexusfs::storage::ChunkStore
            remote_chunk_store{
                root_b
            };

        for (const auto& chunk : chunks)
        {
            require_true(
                remote_chunk_store.contains(
                    chunk.hash
                ),
                "Remote replica existence test"
            );

            require_equal(
                remote_chunk_store.load(
                    chunk.hash
                ),
                chunk.data,
                "Remote replica content test"
            );
        }
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

void test_strict_replication_failure()
{
    TemporaryDirectory directory;

    const std::filesystem::path root =
        directory.path()
        / "strict-node";

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
        "strict-replication-test",
        "127.0.0.1",
        local_port,
        2,
        true,
        {
            {
                "ffffffffffffffffffffffffffffffff",
                "127.0.0.1",
                unavailable_port
            }
        }
    );

    const auto cluster =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    root,
                    "127.0.0.1",
                    local_port
                );

    const nexusfs::app::NexusFsService service{
        root,
        1024,
        cluster,
        2,
        true
    };

    const std::filesystem::path source_path =
        directory.path()
        / "strict-input.bin";

    write_binary_file(
        source_path,
        {
            1,
            2,
            3,
            4,
            5
        }
    );

    require_exception(
        [
            &service,
            &source_path
        ]()
        {
            (void)service.store_file(
                source_path
            );
        },
        "Strict replication failure test"
    );

    const auto catalog =
        service.list_files();

    require_true(
        catalog.files.empty(),
        "Strict replication manifest-withholding test"
    );
}

}

int main()
{
    try
    {
        test_automatic_two_node_replication();

        std::cout
            << "[PASS] Automatic two-node replication\n";

        test_strict_replication_failure();

        std::cout
            << "[PASS] Strict replication failure policy\n";

        std::cout
            << "All NexusFS automatic replication tests passed.\n";

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
