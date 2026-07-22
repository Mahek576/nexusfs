#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/cluster/metadata_catalog.hpp"
#include "nexusfs/cluster/peer_transport.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/manifest_store.hpp"

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
                "nexusfs-metadata-catalog-tests-"
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
            "Failed to create catalog test file."
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
            "Failed to write catalog test file."
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
            "Failed to create catalog test configuration."
        );
    }

    output
        << payload.dump(2)
        << '\n';

    output.close();
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
                "Catalog server startup timed out."
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
perform_unauthorized_catalog_request(
    std::uint16_t port
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
        beast_http::empty_body
    > request{
        beast_http::verb::get,
        "/api/v1/cluster/catalog",
        11
    };

    request.set(
        beast_http::field::host,
        "127.0.0.1"
    );

    request.keep_alive(
        false
    );

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

void test_catalog_codec_validation()
{
    const std::string node_id{
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    };

    std::vector<
        nexusfs::cluster::MetadataCatalogEntry
    > entries{
        {
            std::string(64, 'b'),
            "second.bin",
            1025,
            512,
            3
        },
        {
            std::string(64, 'a'),
            "first.bin",
            512,
            512,
            1
        }
    };

    const auto snapshot =
        nexusfs::cluster::
            MetadataCatalogCodec::create(
                node_id,
                entries
            );

    require_equal(
        snapshot.entries.front().manifest_id,
        std::string(64, 'a'),
        "Canonical catalog ordering test"
    );

    const std::string encoded =
        nexusfs::cluster::
            MetadataCatalogCodec::encode(
                snapshot
            );

    const auto decoded =
        nexusfs::cluster::
            MetadataCatalogCodec::decode(
                encoded,
                node_id
            );

    require_equal(
        decoded.digest,
        snapshot.digest,
        "Catalog digest round-trip test"
    );

    nlohmann::json tampered =
        nlohmann::json::parse(
            encoded
        );

    tampered[
        "entries"
    ][0][
        "file_size"
    ] = 513;

    require_exception(
        [
            &tampered,
            &node_id
        ]()
        {
            (void)nexusfs::cluster::
                MetadataCatalogCodec::decode(
                    tampered.dump(),
                    node_id
                );
        },
        "Tampered catalog rejection test"
    );

    require_exception(
        [
            &encoded
        ]()
        {
            (void)nexusfs::cluster::
                MetadataCatalogCodec::decode(
                    encoded,
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                );
        },
        "Catalog node-identity validation test"
    );

    entries.push_back(
        entries.front()
    );

    require_exception(
        [
            &node_id,
            &entries
        ]()
        {
            (void)nexusfs::cluster::
                MetadataCatalogCodec::create(
                    node_id,
                    entries
                );
        },
        "Duplicate catalog manifest rejection test"
    );
}

void test_real_catalog_transport()
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
        "metadata-catalog-test"
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
            512
        );

    const std::filesystem::path first_path =
        directory.path()
        / "first.bin";

    const std::filesystem::path second_path =
        directory.path()
        / "second.bin";

    write_binary_file(
        first_path,
        std::vector<std::uint8_t>(
            700,
            static_cast<std::uint8_t>(11)
        )
    );

    write_binary_file(
        second_path,
        std::vector<std::uint8_t>(
            1300,
            static_cast<std::uint8_t>(29)
        )
    );

    const auto first_stored =
        service_b->store_file(
            first_path
        );

    const auto second_stored =
        service_b->store_file(
            second_path
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
        nexusfs::cluster::PeerTransport transport{
            cluster_a,
            std::chrono::milliseconds{
                3000
            }
        };

        const auto catalog =
            transport.load_catalog(
                cluster_a
                    ->configuration()
                    .peers
                    .front()
            );

        require_equal(
            catalog.node_id,
            cluster_b->identity().node_id,
            "Remote catalog node identity test"
        );

        require_equal(
            catalog.entries.size(),
            static_cast<std::size_t>(2),
            "Remote catalog entry-count test"
        );

        require_true(
            catalog.entries[0].manifest_id
                < catalog.entries[1].manifest_id,
            "Remote catalog canonical-order test"
        );

        const nexusfs::storage::ManifestStore
            manifest_store{
                root_b
            };

        for (
            const auto& entry :
            catalog.entries
        )
        {
            require_true(
                nexusfs::cluster::
                    MetadataCatalogCodec::
                        entry_matches_manifest(
                            entry,
                            manifest_store.load(
                                entry.manifest_id
                            )
                        ),
                "Catalog entry-to-manifest validation test"
            );
        }

        require_true(
            (
                catalog.entries[0].manifest_id ==
                    first_stored.manifest_id
                || catalog.entries[1].manifest_id ==
                    first_stored.manifest_id
            ),
            "First manifest catalog participation test"
        );

        require_true(
            (
                catalog.entries[0].manifest_id ==
                    second_stored.manifest_id
                || catalog.entries[1].manifest_id ==
                    second_stored.manifest_id
            ),
            "Second manifest catalog participation test"
        );

        const auto unauthorized =
            perform_unauthorized_catalog_request(
                port_b
            );

        require_equal(
            unauthorized.result(),
            beast_http::status::forbidden,
            "Catalog endpoint authorization test"
        );

        nexusfs::http::HttpRouter::Request
            normalized_request{
                beast_http::verb::get,
                "/api/v1/cluster/catalog",
                11
            };

        require_equal(
            router_b.normalized_route(
                normalized_request
            ),
            std::string_view{
                "/api/v1/cluster/catalog"
            },
            "Catalog route normalization test"
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
        test_catalog_codec_validation();

        std::cout
            << "[PASS] Metadata catalog canonical codec\n";

        std::cout
            << "[PASS] Metadata catalog digest validation\n";

        test_real_catalog_transport();

        std::cout
            << "[PASS] Authenticated metadata catalog transport\n";

        std::cout
            << "[PASS] Metadata catalog manifest validation\n";

        std::cout
            << "All NexusFS metadata catalog tests passed.\n";

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
