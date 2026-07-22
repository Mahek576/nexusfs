#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/cluster/metadata_ownership.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"

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
                "nexusfs-dynamic-membership-tests-"
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
    bool thrown = false;

    try
    {
        operation();
    }
    catch (const std::exception&)
    {
        thrown = true;
    }

    require_true(
        thrown,
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

nexusfs::http::HttpRouter::Request
membership_request(
    beast_http::verb method,
    const std::string& target,
    const std::string& cluster_id,
    const std::string& requester_node_id,
    const nlohmann::ordered_json& body
)
{
    nexusfs::http::HttpRouter::Request request{
        method,
        target,
        11
    };

    request.set(
        "X-NexusFS-Cluster-ID",
        cluster_id
    );

    request.set(
        "X-NexusFS-Node-ID",
        requester_node_id
    );

    request.set(
        beast_http::field::content_type,
        "application/json"
    );

    request.keep_alive(
        false
    );

    request.body() =
        body.dump();

    request.prepare_payload();

    return request;
}

void test_dynamic_membership_lifecycle()
{
    TemporaryDirectory directory;

    const std::uint16_t local_port =
        reserve_port();

    const auto foundation =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    directory.path(),
                    "127.0.0.1",
                    local_port
                );

    require_equal(
        foundation->membership_epoch(),
        static_cast<std::uint64_t>(1),
        "Initial membership epoch test"
    );

    require_true(
        foundation
            ->peer_definitions()
            .empty(),
        "Initial standalone membership test"
    );

    const nexusfs::cluster::PeerDefinition
        peer_one{
            std::string(32, 'b'),
            "127.0.0.1",
            reserve_port()
        };

    const nexusfs::cluster::PeerDefinition
        peer_two{
            std::string(32, 'c'),
            "127.0.0.1",
            reserve_port()
        };

    const auto added_one =
        foundation->upsert_peer(
            peer_one,
            1
        );

    require_equal(
        added_one.status,
        nexusfs::cluster::
            MembershipChangeStatus::added,
        "Dynamic member addition test"
    );

    require_equal(
        added_one.epoch,
        static_cast<std::uint64_t>(2),
        "Dynamic member addition epoch test"
    );

    require_true(
        foundation->is_known_peer(
            peer_one.node_id
        ),
        "Dynamic peer visibility test"
    );

    require_equal(
        foundation
            ->configuration()
            .peers
            .size(),
        static_cast<std::size_t>(1),
        "Dynamic configuration projection test"
    );

    foundation->record_peer_heartbeat(
        nexusfs::cluster::HeartbeatMessage{
            foundation
                ->configuration()
                .cluster_id,
            peer_one.node_id,
            peer_one.address,
            peer_one.port,
            1
        }
    );

    require_equal(
        foundation
            ->peer_health()
            .front()
            .state,
        nexusfs::cluster::
            PeerHealthState::healthy,
        "Dynamic peer heartbeat test"
    );

    const auto stale_add =
        foundation->upsert_peer(
            peer_two,
            1
        );

    require_equal(
        stale_add.status,
        nexusfs::cluster::
            MembershipChangeStatus::
                epoch_mismatch,
        "Stale membership epoch rejection test"
    );

    require_true(
        !foundation->is_known_peer(
            peer_two.node_id
        ),
        "Stale membership mutation protection test"
    );

    const auto added_two =
        foundation->upsert_peer(
            peer_two,
            2
        );

    require_equal(
        added_two.epoch,
        static_cast<std::uint64_t>(3),
        "Second member addition epoch test"
    );

    require_exception(
        [
            &foundation,
            &peer_two
        ]()
        {
            (void)foundation->upsert_peer(
                nexusfs::cluster::PeerDefinition{
                    std::string(32, 'd'),
                    peer_two.address,
                    peer_two.port
                },
                3
            );
        },
        "Duplicate membership endpoint rejection test"
    );

    nexusfs::cluster::PeerDefinition
        updated_peer_one =
            peer_one;

    updated_peer_one.port =
        reserve_port();

    const auto updated =
        foundation->upsert_peer(
            updated_peer_one,
            3
        );

    require_equal(
        updated.status,
        nexusfs::cluster::
            MembershipChangeStatus::updated,
        "Dynamic member endpoint update test"
    );

    require_equal(
        updated.epoch,
        static_cast<std::uint64_t>(4),
        "Dynamic member update epoch test"
    );

    const auto removed =
        foundation->remove_peer(
            peer_two.node_id,
            4
        );

    require_equal(
        removed.status,
        nexusfs::cluster::
            MembershipChangeStatus::removed,
        "Dynamic member removal test"
    );

    require_equal(
        removed.epoch,
        static_cast<std::uint64_t>(5),
        "Dynamic member removal epoch test"
    );

    require_true(
        !foundation->is_known_peer(
            peer_two.node_id
        ),
        "Dynamic member removal visibility test"
    );

    const auto restarted =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    directory.path(),
                    "127.0.0.1",
                    local_port
                );

    require_equal(
        restarted->membership_epoch(),
        static_cast<std::uint64_t>(5),
        "Membership restart epoch recovery test"
    );

    require_equal(
        restarted
            ->peer_definitions()
            .size(),
        static_cast<std::size_t>(1),
        "Membership restart peer recovery test"
    );

    require_equal(
        restarted
            ->peer_definitions()
            .front(),
        updated_peer_one,
        "Membership restart endpoint recovery test"
    );

    const nexusfs::cluster::PeerDefinition
        concurrent_peer_one{
            std::string(32, 'd'),
            "127.0.0.1",
            reserve_port()
        };

    const nexusfs::cluster::PeerDefinition
        concurrent_peer_two{
            std::string(32, 'e'),
            "127.0.0.1",
            reserve_port()
        };

    nexusfs::cluster::MembershipChangeResult
        concurrent_result_one;

    nexusfs::cluster::MembershipChangeResult
        concurrent_result_two;

    std::thread first_thread{
        [&]()
        {
            concurrent_result_one =
                restarted->upsert_peer(
                    concurrent_peer_one,
                    5
                );
        }
    };

    std::thread second_thread{
        [&]()
        {
            concurrent_result_two =
                restarted->upsert_peer(
                    concurrent_peer_two,
                    5
                );
        }
    };

    first_thread.join();
    second_thread.join();

    const std::size_t applied_count =
        static_cast<std::size_t>(
            concurrent_result_one.applied
        )
        + static_cast<std::size_t>(
            concurrent_result_two.applied
        );

    require_equal(
        applied_count,
        static_cast<std::size_t>(1),
        "Concurrent membership CAS winner test"
    );

    require_equal(
        restarted->membership_epoch(),
        static_cast<std::uint64_t>(6),
        "Concurrent membership epoch test"
    );

    const auto owners =
        nexusfs::cluster::
            MetadataOwnership::ordered_owners(
                std::string(64, 'a'),
                restarted->identity(),
                restarted->configuration()
            );

    require_equal(
        owners.size(),
        restarted
            ->peer_definitions()
            .size()
            + 1,
        "Dynamic metadata ownership participation test"
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
            512
        );

    const nexusfs::http::HttpRouter router{
        service,
        metrics,
        logger,
        restarted
    };

    const auto list_response =
        router.route(
            nexusfs::http::
                HttpRouter::Request{
                    beast_http::verb::get,
                    "/api/v1/cluster/members",
                    11
                }
        );

    require_equal(
        list_response.result(),
        beast_http::status::ok,
        "Membership listing endpoint test"
    );

    const nlohmann::json list_payload =
        nlohmann::json::parse(
            list_response.body()
        );

    require_equal(
        list_payload.at(
            "membership_epoch"
        ).get<std::uint64_t>(),
        static_cast<std::uint64_t>(6),
        "Membership listing epoch test"
    );

    const auto join_response =
        router.route(
            membership_request(
                beast_http::verb::post,
                "/api/v1/cluster/members/join",
                restarted
                    ->configuration()
                    .cluster_id,
                peer_two.node_id,
                {
                    {
                        "cluster_id",
                        restarted
                            ->configuration()
                            .cluster_id
                    },
                    {
                        "node_id",
                        peer_two.node_id
                    },
                    {
                        "address",
                        peer_two.address
                    },
                    {
                        "port",
                        peer_two.port
                    },
                    {
                        "expected_epoch",
                        6
                    }
                }
            )
        );

    require_equal(
        join_response.result(),
        beast_http::status::created,
        "HTTP dynamic join test"
    );

    require_equal(
        restarted->membership_epoch(),
        static_cast<std::uint64_t>(7),
        "HTTP dynamic join epoch test"
    );

    nexusfs::http::HttpRouter::Request
        manifest_request{
            beast_http::verb::head,
            "/api/v1/cluster/manifests/"
                + std::string(64, 'f'),
            11
        };

    manifest_request.set(
        "X-NexusFS-Cluster-ID",
        restarted
            ->configuration()
            .cluster_id
    );

    manifest_request.set(
        "X-NexusFS-Node-ID",
        peer_two.node_id
    );

    const auto manifest_response =
        router.route(
            manifest_request
        );

    require_equal(
        manifest_response.result(),
        beast_http::status::not_found,
        "Dynamic manifest authorization test"
    );

    nexusfs::http::HttpRouter::Request
        catalog_request{
            beast_http::verb::get,
            "/api/v1/cluster/catalog",
            11
        };

    catalog_request.set(
        "X-NexusFS-Cluster-ID",
        restarted
            ->configuration()
            .cluster_id
    );

    catalog_request.set(
        "X-NexusFS-Node-ID",
        peer_two.node_id
    );

    const auto catalog_response =
        router.route(
            catalog_request
        );

    require_equal(
        catalog_response.result(),
        beast_http::status::ok,
        "Dynamic catalog authorization test"
    );

    const auto leave_response =
        router.route(
            membership_request(
                beast_http::verb::post,
                "/api/v1/cluster/members/leave",
                restarted
                    ->configuration()
                    .cluster_id,
                peer_two.node_id,
                {
                    {
                        "cluster_id",
                        restarted
                            ->configuration()
                            .cluster_id
                    },
                    {
                        "node_id",
                        peer_two.node_id
                    },
                    {
                        "expected_epoch",
                        7
                    }
                }
            )
        );

    require_equal(
        leave_response.result(),
        beast_http::status::ok,
        "HTTP dynamic leave test"
    );

    require_equal(
        restarted->membership_epoch(),
        static_cast<std::uint64_t>(8),
        "HTTP dynamic leave epoch test"
    );

    const auto removed_authorization_response =
        router.route(
            manifest_request
        );

    require_equal(
        removed_authorization_response.result(),
        beast_http::status::forbidden,
        "Removed peer authorization revocation test"
    );

    const nexusfs::cluster::PeerDefinition stale_peer{
        std::string(32, 'f'),
        "127.0.0.1",
        reserve_port()
    };

    const auto conflict_response =
        router.route(
            membership_request(
                beast_http::verb::post,
                "/api/v1/cluster/members/join",
                restarted
                    ->configuration()
                    .cluster_id,
                stale_peer.node_id,
                {
                    {
                        "cluster_id",
                        restarted
                            ->configuration()
                            .cluster_id
                    },
                    {
                        "node_id",
                        stale_peer.node_id
                    },
                    {
                        "address",
                        stale_peer.address
                    },
                    {
                        "port",
                        stale_peer.port
                    },
                    {
                        "expected_epoch",
                        7
                    }
                }
            )
        );

    require_equal(
        conflict_response.result(),
        beast_http::status::conflict,
        "HTTP stale membership epoch test"
    );

    const auto metric_snapshot =
        metrics->snapshot();

    require_equal(
        metric_snapshot
            .membership_join_applied_total,
        static_cast<std::uint64_t>(1),
        "Membership join metric test"
    );

    require_equal(
        metric_snapshot
            .membership_leave_applied_total,
        static_cast<std::uint64_t>(1),
        "Membership leave metric test"
    );

    require_equal(
        metric_snapshot
            .membership_epoch_conflicts_total,
        static_cast<std::uint64_t>(1),
        "Membership epoch-conflict metric test"
    );

    require_true(
        logs.str().find(
            "cluster_membership_join_processed"
        ) != std::string::npos,
        "Membership join structured-log test"
    );

    require_true(
        logs.str().find(
            "cluster_membership_leave_processed"
        ) != std::string::npos,
        "Membership leave structured-log test"
    );

    const auto final_restart =
        nexusfs::cluster::
            ClusterNodeFoundation::
                load_or_create(
                    directory.path(),
                    "127.0.0.1",
                    local_port
                );

    require_equal(
        final_restart->membership_epoch(),
        static_cast<std::uint64_t>(8),
        "Final membership restart epoch test"
    );

    require_true(
        !final_restart->is_known_peer(
            peer_two.node_id
        ),
        "Final membership leave persistence test"
    );

    std::size_t snapshot_files = 0;

    for (
        const std::filesystem::directory_entry& entry :
        std::filesystem::directory_iterator{
            final_restart
                ->cluster_directory()
                / "membership"
        }
    )
    {
        if (entry.is_regular_file())
        {
            ++snapshot_files;
        }
    }

    require_equal(
        snapshot_files,
        static_cast<std::size_t>(8),
        "Immutable membership snapshot-count test"
    );
}

}

int main()
{
    try
    {
        test_dynamic_membership_lifecycle();

        std::cout
            << "[PASS] Durable dynamic membership lifecycle\n";

        std::cout
            << "[PASS] Optimistic membership epoch control\n";

        std::cout
            << "[PASS] Concurrent membership CAS protection\n";

        std::cout
            << "[PASS] Dynamic transport authorization\n";

        std::cout
            << "[PASS] Membership metrics and structured logs\n";

        std::cout
            << "All NexusFS dynamic membership tests passed.\n";

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
