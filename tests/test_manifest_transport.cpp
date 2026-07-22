#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/cluster/peer_transport.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/file_manifest.hpp"
#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/manifest_store.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
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
#include <vector>

namespace
{

namespace asio =
    boost::asio;

namespace beast =
    boost::beast;

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
                "nexusfs-manifest-transport-tests-"
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
            "Failed to create manifest transport configuration."
        );
    }

    output
        << payload.dump(2)
        << '\n';

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write manifest transport configuration."
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
                "Manifest transport server startup timed out."
            );
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds{
                5
            }
        );
    }
}

beast_http::response<
    beast_http::string_body
>
perform_raw_request(
    std::uint16_t port,
    beast_http::verb method,
    const std::string& target,
    const std::string& body,
    const std::string& cluster_id,
    const std::string& node_id
)
{
    asio::io_context context{
        1
    };

    beast::tcp_stream stream{
        context
    };

    stream.expires_after(
        std::chrono::seconds{
            3
        }
    );

    stream.connect(
        Tcp::endpoint{
            asio::ip::make_address(
                "127.0.0.1"
            ),
            port
        }
    );

    beast_http::request<
        beast_http::string_body
    > request{
        method,
        target,
        11
    };

    request.set(
        beast_http::field::host,
        "127.0.0.1"
    );

    if (!cluster_id.empty())
    {
        request.set(
            "X-NexusFS-Cluster-ID",
            cluster_id
        );
    }

    if (!node_id.empty())
    {
        request.set(
            "X-NexusFS-Node-ID",
            node_id
        );
    }

    request.set(
        beast_http::field::content_type,
        "application/octet-stream"
    );

    request.keep_alive(
        false
    );

    request.body() =
        body;

    request.prepare_payload();

    beast_http::write(
        stream,
        request
    );

    beast::flat_buffer buffer;

    beast_http::response<
        beast_http::string_body
    > response;

    beast_http::read(
        stream,
        buffer,
        response
    );

    boost::system::error_code shutdown_error;

    stream.socket().shutdown(
        Tcp::socket::shutdown_both,
        shutdown_error
    );

    return response;
}

void test_real_manifest_transport()
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
        "manifest-transport-test"
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
        const std::vector<std::uint8_t> chunk_data{
            5,
            10,
            15,
            20,
            25
        };

        const std::string chunk_hash =
            nexusfs::storage::
                Sha256Hasher::hash(
                    std::span<
                        const std::uint8_t
                    >{
                        chunk_data.data(),
                        chunk_data.size()
                    }
                );

        const nexusfs::storage::FileManifest manifest =
            nexusfs::storage::FileManifest::restore(
                "transport.bin",
                static_cast<std::uint64_t>(
                    chunk_data.size()
                ),
                chunk_data.size(),
                {
                    chunk_hash
                }
            );

        const std::vector<std::uint8_t>
            encoded_manifest =
                nexusfs::storage::
                    FileManifestCodec::encode(
                        manifest
                    );

        const std::string manifest_id =
            nexusfs::storage::
                Sha256Hasher::hash(
                    std::span<
                        const std::uint8_t
                    >{
                        encoded_manifest.data(),
                        encoded_manifest.size()
                    }
                );

        const auto& peer_b =
            cluster_a
                ->configuration()
                .peers
                .front();

        nexusfs::cluster::PeerTransport transport{
            cluster_a,
            std::chrono::milliseconds{
                3000
            }
        };

        require_true(
            !transport.manifest_exists(
                peer_b,
                manifest_id
            ),
            "Initial remote manifest absence test"
        );

        require_equal(
            transport.store_manifest(
                peer_b,
                manifest_id,
                encoded_manifest
            ),
            nexusfs::cluster::
                RemoteManifestStoreResult::stored,
            "Remote manifest publication test"
        );

        require_true(
            transport.manifest_exists(
                peer_b,
                manifest_id
            ),
            "Remote manifest existence test"
        );

        require_equal(
            transport.store_manifest(
                peer_b,
                manifest_id,
                encoded_manifest
            ),
            nexusfs::cluster::
                RemoteManifestStoreResult::
                    already_exists,
            "Remote manifest idempotency test"
        );

        require_equal(
            transport.load_manifest(
                peer_b,
                manifest_id
            ),
            encoded_manifest,
            "Remote manifest download test"
        );

        const nexusfs::storage::ManifestStore
            receiver_store{
                root_b
            };

        require_true(
            receiver_store.contains(
                manifest_id
            ),
            "Receiver manifest publication test"
        );

        require_equal(
            receiver_store.load(
                manifest_id
            ),
            encoded_manifest,
            "Receiver manifest content test"
        );

        const auto unauthorized_response =
            perform_raw_request(
                port_b,
                beast_http::verb::head,
                "/api/v1/cluster/manifests/"
                    + manifest_id,
                {},
                {},
                {}
            );

        require_equal(
            unauthorized_response.result(),
            beast_http::status::forbidden,
            "Manifest endpoint authorization test"
        );

        std::vector<std::uint8_t>
            noncanonical_manifest =
                encoded_manifest;

        noncanonical_manifest.push_back(
            static_cast<std::uint8_t>(
                '\n'
            )
        );

        const std::string noncanonical_id =
            nexusfs::storage::
                Sha256Hasher::hash(
                    std::span<
                        const std::uint8_t
                    >{
                        noncanonical_manifest.data(),
                        noncanonical_manifest.size()
                    }
                );

        const std::string noncanonical_body{
            reinterpret_cast<const char*>(
                noncanonical_manifest.data()
            ),
            noncanonical_manifest.size()
        };

        const auto noncanonical_response =
            perform_raw_request(
                port_b,
                beast_http::verb::put,
                "/api/v1/cluster/manifests/"
                    + noncanonical_id,
                noncanonical_body,
                cluster_id,
                cluster_a->identity().node_id
            );

        require_equal(
            noncanonical_response.result(),
            beast_http::status::bad_request,
            "Noncanonical manifest rejection test"
        );

        require_true(
            !receiver_store.contains(
                noncanonical_id
            ),
            "Rejected manifest publication-protection test"
        );

        require_exception(
            [
                &transport,
                &peer_b,
                &manifest_id,
                &noncanonical_manifest
            ]()
            {
                (void)transport.store_manifest(
                    peer_b,
                    manifest_id,
                    noncanonical_manifest
                );
            },
            "Client manifest integrity validation test"
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
        test_real_manifest_transport();

        std::cout
            << "[PASS] Authenticated remote manifest transport\n";

        std::cout
            << "[PASS] Canonical manifest validation\n";

        std::cout
            << "[PASS] Manifest publication idempotency\n";

        std::cout
            << "All NexusFS manifest transport tests passed.\n";

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
