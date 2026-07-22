#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/cluster/metadata_ownership.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/chunker.hpp"
#include "nexusfs/storage/file_manifest.hpp"
#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/manifest_store.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

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
#include <optional>
#include <span>
#include <sstream>
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
                "nexusfs-metadata-recovery-tests-"
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

struct RemoteOwnedCandidate
{
    std::filesystem::path source_path;
    std::string manifest_id;
    std::vector<std::uint8_t> source_data;
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
            "Failed to create metadata recovery input."
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
            "Failed to write metadata recovery input."
        );
    }
}

std::vector<std::uint8_t> read_binary_file(
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
            "Failed to read metadata recovery output."
        );
    }

    return std::vector<std::uint8_t>{
        std::istreambuf_iterator<char>{
            input
        },
        std::istreambuf_iterator<char>{}
    };
}

std::filesystem::path manifest_path(
    const std::filesystem::path& root,
    const std::string& manifest_id
)
{
    return (
        root
        / "manifests"
        / manifest_id.substr(
            0,
            2
        )
        / manifest_id.substr(
            2
        )
    );
}

void remove_local_manifest(
    const std::filesystem::path& root,
    const std::string& manifest_id
)
{
    std::error_code removal_error;

    const bool removed =
        std::filesystem::remove(
            manifest_path(
                root,
                manifest_id
            ),
            removal_error
        );

    if (
        removal_error
        || !removed
    )
    {
        throw std::runtime_error(
            "Failed to remove local metadata test manifest."
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
            "Failed to create metadata recovery configuration."
        );
    }

    output
        << payload.dump(2)
        << '\n';

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write metadata recovery configuration."
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
                "Metadata-owner server startup timed out."
            );
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds{
                5
            }
        );
    }
}

RemoteOwnedCandidate find_remote_owned_candidate(
    const std::filesystem::path& directory,
    const nexusfs::cluster::NodeIdentity& identity,
    const nexusfs::cluster::ClusterConfiguration& configuration
)
{
    const nexusfs::storage::Chunker chunker{
        512
    };

    for (
        std::size_t attempt = 0;
        attempt < 512;
        ++attempt
    )
    {
        std::vector<std::uint8_t> data(
            900
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
                        index * 31
                        + attempt * 17
                    )
                    % 251
                );
        }

        const std::filesystem::path source_path =
            directory
            / (
                "metadata-candidate-"
                + std::to_string(
                    attempt
                )
                + ".bin"
            );

        write_binary_file(
            source_path,
            data
        );

        const auto chunks =
            chunker.split_file(
                source_path
            );

        const nexusfs::storage::FileManifest manifest =
            nexusfs::storage::FileManifest::create(
                source_path,
                chunker.chunk_size(),
                chunks
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

        const auto owner =
            nexusfs::cluster::
                MetadataOwnership::select_owner(
                    manifest_id,
                    identity,
                    configuration
                );

        if (!owner.local)
        {
            return RemoteOwnedCandidate{
                source_path,
                manifest_id,
                std::move(data)
            };
        }
    }

    throw std::runtime_error(
        "Failed to generate a remotely owned test manifest."
    );
}

void test_owner_publication_and_recovery()
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
        "metadata-recovery-test"
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

    const RemoteOwnedCandidate candidate =
        find_remote_owned_candidate(
            directory.path(),
            cluster_a->identity(),
            cluster_a->configuration()
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
            1,
            true,
            metrics_a,
            logger_a
        );

    const auto stored =
        service_a->store_file(
            candidate.source_path
        );

    require_equal(
        stored.manifest_id,
        candidate.manifest_id,
        "Metadata publication manifest-ID test"
    );

    require_equal(
        stored.metadata_owner_node_id,
        cluster_b->identity().node_id,
        "Remote metadata-owner identity test"
    );

    require_true(
        !stored.metadata_owner_local,
        "Remote metadata-owner locality test"
    );

    require_true(
        stored.metadata_owner_acknowledged,
        "Strict metadata-owner acknowledgement test"
    );

    const nexusfs::storage::ManifestStore
        remote_manifest_store{
            root_b
        };

    const nexusfs::storage::ManifestStore
        local_manifest_store{
            root_a
        };

    require_true(
        remote_manifest_store.contains(
            stored.manifest_id
        ),
        "Metadata-owner publication test"
    );

    require_true(
        local_manifest_store.contains(
            stored.manifest_id
        ),
        "Local metadata cache publication test"
    );

    try
    {
        remove_local_manifest(
            root_a,
            stored.manifest_id
        );

        const auto verified =
            service_a->verify_file(
                stored.manifest_id
            );

        require_equal(
            verified.total_bytes_verified,
            static_cast<std::uint64_t>(
                candidate.source_data.size()
            ),
            "Recovered metadata verification test"
        );

        require_true(
            local_manifest_store.contains(
                stored.manifest_id
            ),
            "Verification manifest self-healing test"
        );

        remove_local_manifest(
            root_a,
            stored.manifest_id
        );

        const std::filesystem::path output_path =
            directory.path()
            / "metadata-restored.bin";

        const auto restored =
            service_a->restore_file(
                stored.manifest_id,
                output_path
            );

        require_equal(
            restored.bytes_written,
            static_cast<std::uint64_t>(
                candidate.source_data.size()
            ),
            "Recovered metadata restoration-size test"
        );

        require_equal(
            read_binary_file(
                output_path
            ),
            candidate.source_data,
            "Recovered metadata restoration-content test"
        );

        require_true(
            local_manifest_store.contains(
                stored.manifest_id
            ),
            "Restoration manifest self-healing test"
        );

        const auto successful_snapshot =
            metrics_a->snapshot();

        require_equal(
            successful_snapshot
                .metadata_publications_succeeded,
            static_cast<std::uint64_t>(1),
            "Metadata publication success metric test"
        );

        require_equal(
            successful_snapshot
                .remote_manifest_reads_succeeded,
            static_cast<std::uint64_t>(2),
            "Remote manifest-read success metric test"
        );

        require_equal(
            successful_snapshot
                .local_manifest_repairs_total,
            static_cast<std::uint64_t>(2),
            "Local manifest repair metric test"
        );

        require_true(
            logs.str().find(
                "metadata_publication_succeeded"
            ) != std::string::npos,
            "Metadata publication success-log test"
        );

        require_true(
            logs.str().find(
                "metadata_recovery_succeeded"
            ) != std::string::npos,
            "Metadata recovery success-log test"
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

    remove_local_manifest(
        root_a,
        stored.manifest_id
    );

    require_exception(
        [
            &service_a,
            &candidate
        ]()
        {
            (void)service_a->store_file(
                candidate.source_path
            );
        },
        "Unavailable metadata-owner publication test"
    );

    require_true(
        !local_manifest_store.contains(
            stored.manifest_id
        ),
        "Strict metadata publication barrier test"
    );

    const std::filesystem::path failed_output =
        directory.path()
        / "failed-metadata-restore.bin";

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
        "Unavailable metadata-owner recovery test"
    );

    require_true(
        !std::filesystem::exists(
            failed_output
        ),
        "Failed metadata recovery output-protection test"
    );

    const auto final_snapshot =
        metrics_a->snapshot();

    require_equal(
        final_snapshot.metadata_publications_total,
        static_cast<std::uint64_t>(2),
        "Metadata publication total metric test"
    );

    require_equal(
        final_snapshot.metadata_publications_failed,
        static_cast<std::uint64_t>(1),
        "Metadata publication failure metric test"
    );

    require_true(
        final_snapshot.remote_manifest_reads_failed
            >= 1,
        "Remote manifest-read failure metric test"
    );

    require_true(
        logs.str().find(
            "metadata_publication_failed"
        ) != std::string::npos,
        "Metadata publication failure-log test"
    );

    require_true(
        logs.str().find(
            "metadata_recovery_failed"
        ) != std::string::npos,
        "Metadata recovery failure-log test"
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

    require_equal(
        metrics_response.result(),
        beast_http::status::ok,
        "Metadata metrics endpoint status test"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            metrics_response.body()
        );

    require_equal(
        payload
            .at("cluster_transport")
            .at("metadata")
            .at("publications")
            .at("succeeded")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Metadata publication endpoint metric test"
    );

    require_equal(
        payload
            .at("cluster_transport")
            .at("metadata")
            .at("local_repairs")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(2),
        "Metadata repair endpoint metric test"
    );
}

}

int main()
{
    try
    {
        test_owner_publication_and_recovery();

        std::cout
            << "[PASS] Deterministic metadata-owner publication\n";

        std::cout
            << "[PASS] Remote manifest fallback and self-healing\n";

        std::cout
            << "[PASS] Strict metadata-owner acknowledgement\n";

        std::cout
            << "[PASS] Metadata recovery metrics and logs\n";

        std::cout
            << "All NexusFS metadata recovery tests passed.\n";

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
