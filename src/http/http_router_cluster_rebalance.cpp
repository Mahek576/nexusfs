#include "nexusfs/http/http_router.hpp"

#include "nexusfs/cluster/operation_journal.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
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

constexpr std::string_view cluster_header{
    "X-NexusFS-Cluster-ID"
};

constexpr std::string_view node_header{
    "X-NexusFS-Node-ID"
};

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

bool is_authorized_control_request(
    const HttpRouter::Request& request,
    const cluster::ClusterNodeFoundation& cluster_node
)
{
    const auto supplied_cluster =
        request[
            cluster_header
        ];

    const auto supplied_node =
        request[
            node_header
        ];

    if (
        supplied_cluster.empty()
        || supplied_node.empty()
        || supplied_cluster !=
            cluster_node
                .configuration()
                .cluster_id
    )
    {
        return false;
    }

    const std::string_view node_id{
        supplied_node.data(),
        supplied_node.size()
    };

    return (
        node_id ==
            cluster_node
                .identity()
                .node_id
        || cluster_node.is_known_peer(
            node_id
        )
    );
}

nlohmann::ordered_json result_payload(
    const app::RebalanceClusterResult& result
)
{
    return {
        {
            "status",
            result.status
        },
        {
            "operation_id",
            result.operation_id
        },
        {
            "request_digest",
            result.request_digest
        },
        {
            "expected_membership_epoch",
            result.expected_membership_epoch
        },
        {
            "observed_membership_epoch",
            result.observed_membership_epoch
        },
        {
            "replication_factor",
            result.replication_factor
        },
        {
            "manifests_scanned",
            result.manifests_scanned
        },
        {
            "unique_chunks_scanned",
            result.unique_chunks_scanned
        },
        {
            "targets_planned",
            result.targets_planned
        },
        {
            "replicas_observed",
            result.replicas_observed
        },
        {
            "replicas_created",
            result.replicas_created
        },
        {
            "peer_failures",
            result.peer_failures
        },
        {
            "under_replicated_chunks",
            result.under_replicated_chunks
        },
        {
            "converged",
            result.converged
        },
        {
            "replayed",
            result.replayed
        },
        {
            "applied",
            result.applied
        }
    };
}

}

HttpRouter::Response
HttpRouter::route_cluster_rebalance_request(
    const Request& request
) const
{
    if (
        !cluster_node_
        || !is_authorized_control_request(
            request,
            *cluster_node_
        )
    )
    {
        return make_error_response(
            beast_http::status::forbidden,
            "rebalance_not_authorized",
            "The rebalance request did not provide "
            "an authorized cluster identity.",
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
                "The rebalance endpoint supports "
                "only POST.",
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
                "Rebalance body must be a JSON object."
            );
        }

        const std::string operation_id =
            payload.at(
                "operation_id"
            ).get<std::string>();

        const std::uint64_t expected_epoch =
            payload.at(
                "expected_membership_epoch"
            ).get<std::uint64_t>();

        const app::RebalanceClusterResult result =
            service_->rebalance_cluster(
                operation_id,
                expected_epoch
            );

        if (
            result.status ==
            "stale_membership_epoch"
        )
        {
            return make_json_response(
                beast_http::status::conflict,
                result_payload(
                    result
                ),
                request
            );
        }

        return make_json_response(
            result.converged
                ? beast_http::status::ok
                : beast_http::status::multi_status,
            result_payload(
                result
            ),
            request
        );
    }
    catch (
        const cluster::OperationIdConflict&
            error
    )
    {
        return make_error_response(
            beast_http::status::conflict,
            "operation_id_conflict",
            error.what(),
            request
        );
    }
    catch (const nlohmann::json::exception& error)
    {
        return make_error_response(
            beast_http::status::bad_request,
            "invalid_rebalance_json",
            error.what(),
            request
        );
    }
    catch (const std::invalid_argument& error)
    {
        return make_error_response(
            beast_http::status::bad_request,
            "invalid_rebalance_request",
            error.what(),
            request
        );
    }
    catch (const std::exception&)
    {
        return make_error_response(
            beast_http::status::
                internal_server_error,
            "rebalance_failed",
            "The cluster rebalance operation "
            "could not be completed.",
            request
        );
    }
}

}
