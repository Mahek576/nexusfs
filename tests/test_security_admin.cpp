#include "nexusfs/admin/admin_client.hpp"
#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/security/request_security.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
                "nexusfs-security-admin-"
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

class EnvironmentSecurity final
{
public:
    EnvironmentSecurity(
        std::string secret,
        std::string token
    )
    {
        set(
            "NEXUSFS_CLUSTER_SECRET",
            secret
        );

        set(
            "NEXUSFS_ADMIN_TOKEN",
            token
        );
    }

    ~EnvironmentSecurity()
    {
        set(
            "NEXUSFS_CLUSTER_SECRET",
            ""
        );

        set(
            "NEXUSFS_ADMIN_TOKEN",
            ""
        );
    }

private:
    static void set(
        const char* name,
        const std::string& value
    )
    {
#ifdef _WIN32
        if (
            _putenv_s(
                name,
                value.c_str()
            )
            != 0
        )
        {
            throw std::runtime_error(
                "Failed to update the test environment."
            );
        }
#else
        if (value.empty())
        {
            if (unsetenv(name) != 0)
            {
                throw std::runtime_error(
                    "Failed to clear the test environment."
                );
            }
        }
        else if (
            setenv(
                name,
                value.c_str(),
                1
            )
            != 0
        )
        {
            throw std::runtime_error(
                "Failed to update the test environment."
            );
        }
#endif
    }
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

void write_file(
    const std::filesystem::path& path
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
            "Failed to create the security test file."
        );
    }

    for (
        std::size_t index = 0;
        index < 1600;
        ++index
    )
    {
        const char value =
            static_cast<char>(
                index % 251
            );

        output.write(
            &value,
            1
        );
    }

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write the security test file."
        );
    }
}

nexusfs::http::HttpRouter::Request
admin_request(
    beast_http::verb method,
    std::string target,
    std::string token,
    std::string body = {}
)
{
    nexusfs::http::HttpRouter::Request request{
        method,
        std::move(target),
        11
    };

    if (!token.empty())
    {
        request.set(
            beast_http::field::authorization,
            "Bearer "
                + token
        );
    }

    if (
        method ==
        beast_http::verb::post
    )
    {
        request.set(
            beast_http::field::content_type,
            "application/json"
        );

        request.body() =
            std::move(body);
    }

    request.keep_alive(
        false
    );

    request.prepare_payload();

    return request;
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
                "Security/admin server startup timed out."
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

void test_security_and_admin_control_plane()
{
    const std::string cluster_secret{
        "nexusfs-test-cluster-secret-0123456789abcdef"
    };

    const std::string admin_token{
        "nexusfs-test-admin-token-0123456789"
    };

    EnvironmentSecurity environment{
        cluster_secret,
        admin_token
    };

    TemporaryDirectory directory;

    const std::uint16_t port =
        reserve_port();

    const auto cluster_node =
        nexusfs::cluster::
            ClusterNodeFoundation::load_or_create(
                directory.path(),
                "127.0.0.1",
                port
            );

    const auto metrics =
        std::make_shared<
            nexusfs::observability::
                MetricsRegistry
        >();

    std::ostringstream logs;

    const auto logger =
        std::make_shared<
            nexusfs::observability::
                JsonLogger
        >(
            &logs
        );

    const auto service =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            directory.path(),
            256,
            cluster_node,
            1,
            true,
            metrics,
            logger
        );

    const std::filesystem::path source_file =
        directory.path()
        / "secured-file.bin";

    write_file(
        source_file
    );

    const auto stored =
        service->store_file(
            source_file
        );

    require_true(
        !stored.manifest_id.empty(),
        "Secured storage setup test"
    );

    const nexusfs::http::HttpRouter router{
        service,
        metrics,
        logger,
        cluster_node
    };

    nexusfs::http::HttpRouter::Request
        unsigned_cluster_request{
            beast_http::verb::get,
            "/api/v1/cluster",
            11
        };

    unsigned_cluster_request.keep_alive(
        false
    );

    const auto unsigned_response =
        router.route(
            unsigned_cluster_request
        );

    require_equal(
        unsigned_response.result(),
        beast_http::status::unauthorized,
        "Unsigned peer request rejection test"
    );

    nexusfs::security::RequestSecurity signer{
        cluster_secret,
        admin_token
    };

    nexusfs::http::HttpRouter::Request
        signed_cluster_request{
            beast_http::verb::get,
            "/api/v1/cluster",
            11
        };

    signed_cluster_request.keep_alive(
        false
    );

    signed_cluster_request.prepare_payload();

    signer.sign_peer_request(
        signed_cluster_request,
        cluster_node
            ->configuration()
            .cluster_id,
        cluster_node
            ->identity()
            .node_id
    );

    const auto signed_response =
        router.route(
            signed_cluster_request
        );

    require_equal(
        signed_response.result(),
        beast_http::status::ok,
        "Signed peer request acceptance test"
    );

    const auto replay_response =
        router.route(
            signed_cluster_request
        );

    require_equal(
        replay_response.result(),
        beast_http::status::conflict,
        "Peer replay rejection test"
    );

    nexusfs::http::HttpRouter::Request
        tampered_request{
            beast_http::verb::get,
            "/api/v1/cluster",
            11
        };

    tampered_request.keep_alive(
        false
    );

    tampered_request.prepare_payload();

    signer.sign_peer_request(
        tampered_request,
        cluster_node
            ->configuration()
            .cluster_id,
        cluster_node
            ->identity()
            .node_id
    );

    tampered_request.body() =
        "tampered-after-signing";

    const auto tampered_response =
        router.route(
            tampered_request
        );

    require_equal(
        tampered_response.result(),
        beast_http::status::unauthorized,
        "Signed body tampering rejection test"
    );

    const auto no_admin_token_response =
        router.route(
            admin_request(
                beast_http::verb::get,
                "/api/v1/admin/overview",
                {}
            )
        );

    require_equal(
        no_admin_token_response.result(),
        beast_http::status::unauthorized,
        "Missing admin token rejection test"
    );

    const auto wrong_admin_token_response =
        router.route(
            admin_request(
                beast_http::verb::get,
                "/api/v1/admin/overview",
                "incorrect-admin-token"
            )
        );

    require_equal(
        wrong_admin_token_response.result(),
        beast_http::status::unauthorized,
        "Invalid admin token rejection test"
    );

    const auto overview_response =
        router.route(
            admin_request(
                beast_http::verb::get,
                "/api/v1/admin/overview",
                admin_token
            )
        );

    require_equal(
        overview_response.result(),
        beast_http::status::ok,
        "Admin overview status test"
    );

    const nlohmann::json overview =
        nlohmann::json::parse(
            overview_response.body()
        );

    require_true(
        overview.at(
            "security"
        ).at(
            "peer_request_signing"
        ).get<bool>(),
        "Admin peer-security visibility test"
    );

    require_true(
        overview.at(
            "security"
        ).at(
            "admin_authentication"
        ).get<bool>(),
        "Admin authentication visibility test"
    );

    require_equal(
        overview.at(
            "storage"
        ).at(
            "manifests"
        ).get<std::size_t>(),
        static_cast<std::size_t>(1),
        "Admin storage summary test"
    );

    const auto files_response =
        router.route(
            admin_request(
                beast_http::verb::get,
                "/api/v1/admin/files",
                admin_token
            )
        );

    require_equal(
        files_response.result(),
        beast_http::status::ok,
        "Admin file catalog status test"
    );

    const nlohmann::json files =
        nlohmann::json::parse(
            files_response.body()
        );

    require_equal(
        files.at(
            "count"
        ).get<std::size_t>(),
        static_cast<std::size_t>(1),
        "Admin file catalog count test"
    );

    const auto sync_response =
        router.route(
            admin_request(
                beast_http::verb::post,
                "/api/v1/admin/operations/catalog-sync",
                admin_token
            )
        );

    require_equal(
        sync_response.result(),
        beast_http::status::ok,
        "Admin catalog sync test"
    );

    const auto repair_response =
        router.route(
            admin_request(
                beast_http::verb::post,
                "/api/v1/admin/operations/repair",
                admin_token
            )
        );

    require_equal(
        repair_response.result(),
        beast_http::status::ok,
        "Admin replica repair test"
    );

    const auto maintenance_response =
        router.route(
            admin_request(
                beast_http::verb::post,
                "/api/v1/admin/operations/maintenance",
                admin_token
            )
        );

    require_equal(
        maintenance_response.result(),
        beast_http::status::ok,
        "Admin maintenance control test"
    );

    const auto rebalance_response =
        router.route(
            admin_request(
                beast_http::verb::post,
                "/api/v1/admin/operations/rebalance",
                admin_token,
                nlohmann::ordered_json{
                    {
                        "operation_id",
                        "admin-security-rebalance"
                    },
                    {
                        "expected_membership_epoch",
                        cluster_node
                            ->membership_epoch()
                    }
                }.dump()
            )
        );

    require_equal(
        rebalance_response.result(),
        beast_http::status::ok,
        "Admin rebalance control test"
    );

    nexusfs::http::HttpServer server{
        "127.0.0.1",
        port,
        router
    };

    std::exception_ptr server_exception;

    std::thread server_thread{
        [&]()
        {
            try
            {
                server.run();
            }
            catch (...)
            {
                server_exception =
                    std::current_exception();
            }
        }
    };

    wait_for_server(
        server
    );

    try
    {
        const nexusfs::admin::AdminClient client{
            "127.0.0.1",
            port,
            admin_token
        };

        const auto client_overview =
            client.get(
                "/api/v1/admin/overview"
            );

        require_true(
            client_overview.successful(),
            "Admin client overview request test"
        );

        const auto client_files =
            client.get(
                "/api/v1/admin/files"
            );

        require_true(
            client_files.successful(),
            "Admin client file request test"
        );

        const auto client_sync =
            client.post(
                "/api/v1/admin/operations/catalog-sync"
            );

        require_true(
            client_sync.successful(),
            "Admin client operation request test"
        );
    }
    catch (...)
    {
        stop_and_join(
            server,
            server_thread
        );

        throw;
    }

    stop_and_join(
        server,
        server_thread
    );

    if (server_exception)
    {
        std::rethrow_exception(
            server_exception
        );
    }

    const auto snapshot =
        metrics->snapshot();

    require_true(
        snapshot.peer_security_requests_total
            >= 4,
        "Peer security request metric test"
    );

    require_true(
        snapshot.peer_security_requests_accepted
            >= 1,
        "Peer security acceptance metric test"
    );

    require_true(
        snapshot.peer_security_requests_rejected
            >= 3,
        "Peer security rejection metric test"
    );

    require_true(
        snapshot.peer_security_replays_rejected
            >= 1,
        "Peer replay metric test"
    );

    require_true(
        snapshot.admin_security_requests_accepted
            >= 9,
        "Admin security acceptance metric test"
    );

    require_true(
        snapshot.admin_security_requests_rejected
            >= 2,
        "Admin security rejection metric test"
    );

    require_true(
        logs.str().find(
            "peer_request_rejected"
        ) != std::string::npos,
        "Peer security audit log test"
    );

    require_true(
        logs.str().find(
            "admin_request_rejected"
        ) != std::string::npos,
        "Admin rejection audit log test"
    );

    require_true(
        logs.str().find(
            "admin_request_accepted"
        ) != std::string::npos,
        "Admin acceptance audit log test"
    );
}

}

int main()
{
    try
    {
        test_security_and_admin_control_plane();

        std::cout
            << "[PASS] HMAC peer request authentication\n";

        std::cout
            << "[PASS] Timestamp, nonce and replay protection\n";

        std::cout
            << "[PASS] Constant-time admin authentication\n";

        std::cout
            << "[PASS] Dashboard-ready admin overview and files\n";

        std::cout
            << "[PASS] Admin sync, repair, maintenance and rebalance\n";

        std::cout
            << "[PASS] NexusFS administrator HTTP client\n";

        std::cout
            << "[PASS] Security metrics and audit logging\n";

        std::cout
            << "All NexusFS security and admin tests passed.\n";

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
