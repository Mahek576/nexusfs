#include "nexusfs/http/http_router.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace nexusfs::http
{

namespace beast_http =
    boost::beast::http;

namespace
{

constexpr std::string_view members_route{
    "/api/v1/cluster/members"
};

constexpr std::string_view join_route{
    "/api/v1/cluster/members/join"
};

constexpr std::string_view leave_route{
    "/api/v1/cluster/members/leave"
};

constexpr std::string_view cluster_header{
    "X-NexusFS-Cluster-ID"
};

constexpr std::string_view node_header{
    "X-NexusFS-Node-ID"
};

std::string_view request_target(
    const HttpRouter::Request& request
) noexcept
{
    const auto target =
        request.target();

    return std::string_view{
        target.data(),
        target.size()
    };
}

HttpRouter::Response make_json_response(
    beast_http::status status,
    const nlohmann::ordered_json& payload,
    const HttpRouter::Request& request
)
{
    HttpRouter::Response response{
        status,
        request.version()
    };

    response.set(
        beast_http::field::server,
        "NexusFS"
    );

    response.set(
        beast_http::field::content_type,
        "application/json"
    );

    response.set(
        beast_http::field::cache_control,
        "no-store"
    );

    response.keep_alive(
        request.keep_alive()
    );

    response.body() =
        payload.dump();

    response.prepare_payload();

    return response;
}

HttpRouter::Response make_error_response(
    beast_http::status status,
    std::string code,
    std::string message,
    const HttpRouter::Request& request
)
{
    return make_json_response(
        status,
        {
            {
                "error",
                {
                    {
                        "code",
                        std::move(code)
                    },
                    {
                        "message",
                        std::move(message)
                    }
                }
            }
        },
        request
    );
}

bool cluster_header_matches(
    const HttpRouter::Request& request,
    const cluster::ClusterNodeFoundation& cluster_node
)
{
    const auto supplied_cluster =
        request[
            cluster_header
        ];

    return (
        !supplied_cluster.empty()
        && supplied_cluster ==
            cluster_node
                .configuration()
                .cluster_id
    );
}

bool requester_can_change_member(
    const HttpRouter::Request& request,
    const cluster::ClusterNodeFoundation& cluster_node,
    std::string_view target_node_id
)
{
    if (
        !cluster_header_matches(
            request,
            cluster_node
        )
    )
    {
        return false;
    }

    const auto supplied_node =
        request[
            node_header
        ];

    if (supplied_node.empty())
    {
        return false;
    }

    const std::string_view requester{
        supplied_node.data(),
        supplied_node.size()
    };

    return (
        requester == target_node_id
        || cluster_node.is_known_peer(
            requester
        )
    );
}

nlohmann::ordered_json member_payload(
    const cluster::PeerHealthSnapshot& member
)
{
    return {
        {
            "node_id",
            member.peer.node_id
        },
        {
            "address",
            member.peer.address
        },
        {
            "port",
            member.peer.port
        },
        {
            "state",
            cluster::peer_health_state_name(
                member.state
            )
        },
        {
            "successful_heartbeats",
            member.successful_heartbeats
        },
        {
            "consecutive_failures",
            member.consecutive_failures
        },
        {
            "last_seen_unix_ms",
            member.last_seen_unix_ms
        },
        {
            "last_error",
            member.last_error
        }
    };
}

std::uint16_t parse_port(
    const nlohmann::json& payload
)
{
    const std::uint64_t value =
        payload.at(
            "port"
        ).get<std::uint64_t>();

    if (
        value == 0
        || value >
            static_cast<std::uint64_t>(
                std::numeric_limits<
                    std::uint16_t
                >::max()
            )
    )
    {
        throw std::invalid_argument(
            "Membership port must be between "
            "1 and 65535."
        );
    }

    return static_cast<std::uint16_t>(
        value
    );
}

}

HttpRouter::Response
HttpRouter::route_cluster_membership_request(
    const Request& request
) const
{
    const std::string_view target =
        request_target(
            request
        );

    if (target == members_route)
    {
        if (
            request.method() !=
            beast_http::verb::get
        )
        {
            HttpRouter::Response response =
                make_error_response(
                    beast_http::status::
                        method_not_allowed,
                    "method_not_allowed",
                    "The membership listing endpoint "
                    "supports only GET.",
                    request
                );

            response.set(
                beast_http::field::allow,
                "GET"
            );

            return response;
        }

        const cluster::ClusterSnapshot snapshot =
            cluster_node_->snapshot();

        nlohmann::ordered_json members =
            nlohmann::ordered_json::array();

        for (
            const cluster::PeerHealthSnapshot& member :
            snapshot.peers
        )
        {
            members.push_back(
                member_payload(
                    member
                )
            );
        }

        return make_json_response(
            beast_http::status::ok,
            {
                {
                    "cluster_id",
                    snapshot.configuration
                        .cluster_id
                },
                {
                    "membership_epoch",
                    snapshot.membership_epoch
                },
                {
                    "member_count",
                    snapshot.peers.size()
                },
                {
                    "members",
                    std::move(members)
                }
            },
            request
        );
    }

    const bool join_request =
        target == join_route;

    const bool leave_request =
        target == leave_route;

    if (
        !join_request
        && !leave_request
    )
    {
        return make_error_response(
            beast_http::status::not_found,
            "membership_route_not_found",
            "The requested membership route "
            "does not exist.",
            request
        );
    }

    if (
        request.method() !=
        beast_http::verb::post
    )
    {
        HttpRouter::Response response =
            make_error_response(
                beast_http::status::
                    method_not_allowed,
                "method_not_allowed",
                "Membership changes support only POST.",
                request
            );

        response.set(
            beast_http::field::allow,
            "POST"
        );

        return response;
    }

    try
    {
        const nlohmann::json payload =
            nlohmann::json::parse(
                request.body()
            );

        if (!payload.is_object())
        {
            throw std::invalid_argument(
                "Membership request body must be "
                "a JSON object."
            );
        }

        const std::string cluster_id =
            payload.at(
                "cluster_id"
            ).get<std::string>();

        if (
            cluster_id !=
            cluster_node_
                ->configuration()
                .cluster_id
        )
        {
            metrics_registry_->
                record_membership_change(
                    join_request,
                    false,
                    false
                );

            return make_error_response(
                beast_http::status::forbidden,
                "cluster_mismatch",
                "Membership request belongs to "
                "a different cluster.",
                request
            );
        }

        const std::string node_id =
            payload.at(
                "node_id"
            ).get<std::string>();

        if (
            !requester_can_change_member(
                request,
                *cluster_node_,
                node_id
            )
        )
        {
            metrics_registry_->
                record_membership_change(
                    join_request,
                    false,
                    false
                );

            return make_error_response(
                beast_http::status::forbidden,
                "membership_not_authorized",
                "The membership request did not provide "
                "an authorized cluster identity.",
                request
            );
        }

        const std::uint64_t expected_epoch =
            payload.at(
                "expected_epoch"
            ).get<std::uint64_t>();

        cluster::MembershipChangeResult result;

        if (join_request)
        {
            result =
                cluster_node_->upsert_peer(
                    cluster::PeerDefinition{
                        node_id,
                        payload.at(
                            "address"
                        ).get<std::string>(),
                        parse_port(
                            payload
                        )
                    },
                    expected_epoch
                );
        }
        else
        {
            result =
                cluster_node_->remove_peer(
                    node_id,
                    expected_epoch
                );
        }

        const bool epoch_conflict =
            result.status ==
            cluster::MembershipChangeStatus::
                epoch_mismatch;

        metrics_registry_->
            record_membership_change(
                join_request,
                result.applied,
                epoch_conflict
            );

        if (epoch_conflict)
        {
            return make_json_response(
                beast_http::status::conflict,
                {
                    {
                        "status",
                        cluster::
                            membership_change_status_name(
                                result.status
                            )
                    },
                    {
                        "expected_epoch",
                        expected_epoch
                    },
                    {
                        "current_epoch",
                        result.epoch
                    },
                    {
                        "member_count",
                        result.member_count
                    }
                },
                request
            );
        }

        if (
            result.status ==
            cluster::MembershipChangeStatus::
                not_found
        )
        {
            return make_error_response(
                beast_http::status::not_found,
                "member_not_found",
                "The requested peer is not a "
                "cluster member.",
                request
            );
        }

        logger_->log(
            result.applied
                ? observability::LogLevel::info
                : observability::LogLevel::debug,
            join_request
                ? "cluster_membership_join_processed"
                : "cluster_membership_leave_processed",
            join_request
                ? "A dynamic cluster membership join "
                  "was processed."
                : "A dynamic cluster membership leave "
                  "was processed.",
            {
                observability::LogField{
                    "peer_node_id",
                    node_id
                },
                observability::LogField{
                    "status",
                    std::string{
                        cluster::
                            membership_change_status_name(
                                result.status
                            )
                    }
                },
                observability::LogField{
                    "membership_epoch",
                    result.epoch
                },
                observability::LogField{
                    "member_count",
                    static_cast<std::uint64_t>(
                        result.member_count
                    )
                },
                observability::LogField{
                    "applied",
                    result.applied
                }
            }
        );

        const beast_http::status response_status =
            result.status ==
                cluster::MembershipChangeStatus::
                    added
            ? beast_http::status::created
            : beast_http::status::ok;

        return make_json_response(
            response_status,
            {
                {
                    "status",
                    cluster::
                        membership_change_status_name(
                            result.status
                        )
                },
                {
                    "membership_epoch",
                    result.epoch
                },
                {
                    "member_count",
                    result.member_count
                },
                {
                    "applied",
                    result.applied
                },
                {
                    "node_id",
                    node_id
                }
            },
            request
        );
    }
    catch (const std::invalid_argument& error)
    {
        metrics_registry_->
            record_membership_change(
                join_request,
                false,
                false
            );

        return make_error_response(
            beast_http::status::bad_request,
            "invalid_membership_request",
            error.what(),
            request
        );
    }
    catch (const nlohmann::json::exception& error)
    {
        metrics_registry_->
            record_membership_change(
                join_request,
                false,
                false
            );

        return make_error_response(
            beast_http::status::bad_request,
            "invalid_membership_json",
            error.what(),
            request
        );
    }
    catch (const std::exception&)
    {
        metrics_registry_->
            record_membership_change(
                join_request,
                false,
                false
            );

        return make_error_response(
            beast_http::status::
                internal_server_error,
            "membership_change_failed",
            "The dynamic membership change "
            "could not be completed.",
            request
        );
    }
}

}
