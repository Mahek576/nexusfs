#include "nexusfs/http/http_router.hpp"

#include "nexusfs/cluster/operation_journal.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
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

constexpr std::string_view overview_route{
    "/api/v1/admin/overview"
};

constexpr std::string_view files_route{
    "/api/v1/admin/files"
};

constexpr std::string_view sync_route{
    "/api/v1/admin/operations/catalog-sync"
};

constexpr std::string_view repair_route{
    "/api/v1/admin/operations/repair"
};

constexpr std::string_view maintenance_route{
    "/api/v1/admin/operations/maintenance"
};

constexpr std::string_view rebalance_route{
    "/api/v1/admin/operations/rebalance"
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

    response.set(
        "X-Content-Type-Options",
        "nosniff"
    );

    response.set(
        "Content-Security-Policy",
        "default-src 'none'"
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

HttpRouter::Response make_method_response(
    std::string allowed,
    const HttpRouter::Request& request
)
{
    HttpRouter::Response response =
        make_error_response(
            beast_http::status::
                method_not_allowed,
            "method_not_allowed",
            "The requested admin endpoint does not "
            "support this HTTP method.",
            request
        );

    response.set(
        beast_http::field::allow,
        std::move(allowed)
    );

    return response;
}

nlohmann::ordered_json file_payload(
    const app::StoredFileSummary& file
)
{
    return {
        {
            "manifest_id",
            file.manifest_id
        },
        {
            "original_filename",
            file.original_filename
        },
        {
            "file_size",
            file.file_size
        },
        {
            "chunk_size",
            file.configured_chunk_size
        },
        {
            "chunk_count",
            file.chunk_count
        },
        {
            "missing_chunks",
            file.missing_chunks
        },
        {
            "complete",
            file.missing_chunks == 0
        }
    };
}

nlohmann::ordered_json files_payload(
    const app::ListFilesResult& files
)
{
    nlohmann::ordered_json entries =
        nlohmann::ordered_json::array();

    for (
        const app::StoredFileSummary& file :
        files.files
    )
    {
        entries.push_back(
            file_payload(
                file
            )
        );
    }

    return {
        {
            "count",
            files.files.size()
        },
        {
            "complete_manifests",
            files.complete_manifests
        },
        {
            "incomplete_manifests",
            files.incomplete_manifests
        },
        {
            "files",
            std::move(entries)
        }
    };
}

nlohmann::ordered_json cluster_payload(
    const std::shared_ptr<
        cluster::ClusterNodeFoundation
    >& cluster_node
)
{
    if (!cluster_node)
    {
        return {
            {
                "enabled",
                false
            }
        };
    }

    const cluster::ClusterSnapshot snapshot =
        cluster_node->snapshot();

    nlohmann::ordered_json peers =
        nlohmann::ordered_json::array();

    for (
        const cluster::PeerHealthSnapshot& peer :
        snapshot.peers
    )
    {
        peers.push_back(
            {
                {
                    "node_id",
                    peer.peer.node_id
                },
                {
                    "address",
                    peer.peer.address
                },
                {
                    "port",
                    peer.peer.port
                },
                {
                    "state",
                    cluster::peer_health_state_name(
                        peer.state
                    )
                },
                {
                    "last_seen_unix_ms",
                    peer.last_seen_unix_ms
                },
                {
                    "consecutive_failures",
                    peer.consecutive_failures
                },
                {
                    "last_error",
                    peer.last_error
                }
            }
        );
    }

    return {
        {
            "enabled",
            true
        },
        {
            "cluster_id",
            snapshot.configuration.cluster_id
        },
        {
            "node_id",
            snapshot.local_identity.node_id
        },
        {
            "membership_epoch",
            snapshot.membership_epoch
        },
        {
            "replication_factor",
            snapshot.configuration
                .replication_factor
        },
        {
            "strict_replication",
            snapshot.configuration
                .strict_replication
        },
        {
            "configured_peers",
            snapshot.peers.size()
        },
        {
            "healthy_peers",
            snapshot.healthy_peers
        },
        {
            "suspect_peers",
            snapshot.suspect_peers
        },
        {
            "unavailable_peers",
            snapshot.unavailable_peers
        },
        {
            "unknown_peers",
            snapshot.unknown_peers
        },
        {
            "peers",
            std::move(peers)
        }
    };
}

}

HttpRouter::Response
HttpRouter::route_admin_request(
    const Request& request
) const
{
    const security::AuthenticationResult authentication =
        request_security_->
            verify_admin_request(
                request
            );

    metrics_registry_->
        record_admin_security_result(
            authentication.accepted
        );

    if (!authentication.accepted)
    {
        logger_->log(
            observability::LogLevel::warning,
            "admin_request_rejected",
            "An administrative HTTP request "
            "was rejected.",
            {
                observability::LogField{
                    "status",
                    std::string{
                        security::
                            authentication_status_name(
                                authentication.status
                            )
                    }
                },
                observability::LogField{
                    "route",
                    std::string{
                        request_target(
                            request
                        )
                    }
                }
            }
        );

        HttpRouter::Response response =
            make_error_response(
                authentication.status ==
                    security::AuthenticationStatus::
                        admin_disabled
                    ? beast_http::status::
                          service_unavailable
                    : beast_http::status::
                          unauthorized,
                authentication.status ==
                    security::AuthenticationStatus::
                        admin_disabled
                    ? "admin_authentication_disabled"
                    : "admin_not_authorized",
                authentication.reason,
                request
            );

        if (
            authentication.status !=
            security::AuthenticationStatus::
                admin_disabled
        )
        {
            response.set(
                beast_http::field::
                    www_authenticate,
                "Bearer"
            );
        }

        return response;
    }

    const std::string_view target =
        request_target(
            request
        );

    logger_->log(
        observability::LogLevel::debug,
        "admin_request_accepted",
        "An authenticated administrative HTTP "
        "request was accepted.",
        {
            observability::LogField{
                "route",
                std::string{
                    target
                }
            }
        }
    );

    if (target == overview_route)
    {
        if (
            request.method() !=
            beast_http::verb::get
        )
        {
            return make_method_response(
                "GET",
                request
            );
        }

        try
        {
            const app::ListFilesResult files =
                service_->list_files();

            std::uint64_t logical_bytes =
                0;

            std::uint64_t chunk_references =
                0;

            std::uint64_t missing_chunks =
                0;

            for (
                const app::StoredFileSummary& file :
                files.files
            )
            {
                logical_bytes +=
                    file.file_size;

                chunk_references +=
                    static_cast<std::uint64_t>(
                        file.chunk_count
                    );

                missing_chunks +=
                    static_cast<std::uint64_t>(
                        file.missing_chunks
                    );
            }

            const observability::MetricsSnapshot metrics =
                metrics_registry_->snapshot();

            return make_json_response(
                beast_http::status::ok,
                {
                    {
                        "service",
                        {
                            {
                                "name",
                                "NexusFS"
                            },
                            {
                                "api_version",
                                "v1"
                            },
                            {
                                "status",
                                "online"
                            },
                            {
                                "storage_root",
                                service_
                                    ->storage_root()
                                    .string()
                            },
                            {
                                "chunk_size",
                                service_
                                    ->default_chunk_size()
                            },
                            {
                                "uptime_milliseconds",
                                metrics
                                    .uptime_milliseconds
                            }
                        }
                    },
                    {
                        "security",
                        {
                            {
                                "peer_request_signing",
                                request_security_
                                    ->peer_signing_enabled()
                            },
                            {
                                "admin_authentication",
                                request_security_
                                    ->admin_authentication_enabled()
                            },
                            {
                                "peer_requests_accepted",
                                metrics
                                    .peer_security_requests_accepted
                            },
                            {
                                "peer_requests_rejected",
                                metrics
                                    .peer_security_requests_rejected
                            },
                            {
                                "peer_replays_rejected",
                                metrics
                                    .peer_security_replays_rejected
                            },
                            {
                                "admin_requests_accepted",
                                metrics
                                    .admin_security_requests_accepted
                            },
                            {
                                "admin_requests_rejected",
                                metrics
                                    .admin_security_requests_rejected
                            }
                        }
                    },
                    {
                        "storage",
                        {
                            {
                                "manifests",
                                files.files.size()
                            },
                            {
                                "complete_manifests",
                                files.complete_manifests
                            },
                            {
                                "incomplete_manifests",
                                files.incomplete_manifests
                            },
                            {
                                "logical_bytes",
                                logical_bytes
                            },
                            {
                                "chunk_references",
                                chunk_references
                            },
                            {
                                "missing_chunks",
                                missing_chunks
                            }
                        }
                    },
                    {
                        "cluster",
                        cluster_payload(
                            cluster_node_
                        )
                    },
                    {
                        "http",
                        {
                            {
                                "requests_total",
                                metrics.requests_total
                            },
                            {
                                "requests_in_flight",
                                metrics.requests_in_flight
                            },
                            {
                                "requests_succeeded",
                                metrics.requests_succeeded
                            },
                            {
                                "requests_failed",
                                metrics.requests_failed
                            },
                            {
                                "connections_active",
                                metrics.connections_active
                            }
                        }
                    },
                    {
                        "operations",
                        {
                            {
                                "catalog_sync_runs",
                                metrics
                                    .metadata_catalog_sync_runs_total
                            },
                            {
                                "repair_runs",
                                metrics
                                    .replica_maintenance_runs_total
                            },
                            {
                                "rebalance_runs",
                                metrics
                                    .rebalancing_runs_total
                            },
                            {
                                "rebalance_replays",
                                metrics
                                    .rebalancing_replayed_total
                            },
                            {
                                "rebalance_stale_epoch",
                                metrics
                                    .rebalancing_stale_epoch_total
                            }
                        }
                    }
                },
                request
            );
        }
        catch (const std::exception&)
        {
            return make_error_response(
                beast_http::status::
                    internal_server_error,
                "admin_overview_failed",
                "The administrator overview could "
                "not be generated.",
                request
            );
        }
    }

    if (target == files_route)
    {
        if (
            request.method() !=
            beast_http::verb::get
        )
        {
            return make_method_response(
                "GET",
                request
            );
        }

        try
        {
            return make_json_response(
                beast_http::status::ok,
                files_payload(
                    service_->list_files()
                ),
                request
            );
        }
        catch (const std::exception&)
        {
            return make_error_response(
                beast_http::status::
                    internal_server_error,
                "admin_files_failed",
                "The administrator file catalog "
                "could not be generated.",
                request
            );
        }
    }

    if (
        target == sync_route
        || target == repair_route
        || target == maintenance_route
        || target == rebalance_route
    )
    {
        if (
            request.method() !=
            beast_http::verb::post
        )
        {
            return make_method_response(
                "POST",
                request
            );
        }

        if (!cluster_node_)
        {
            return make_error_response(
                beast_http::status::
                    service_unavailable,
                "cluster_not_configured",
                "The requested administrator operation "
                "requires cluster services.",
                request
            );
        }
    }

    try
    {
        if (target == sync_route)
        {
            const app::SynchronizeMetadataCatalogResult
                result =
                    service_->
                        synchronize_metadata_catalog();

            return make_json_response(
                result.converged
                    ? beast_http::status::ok
                    : beast_http::status::
                          multi_status,
                {
                    {
                        "operation",
                        "catalog_sync"
                    },
                    {
                        "peers_contacted",
                        result.peers_contacted
                    },
                    {
                        "peers_succeeded",
                        result.peers_succeeded
                    },
                    {
                        "peers_failed",
                        result.peers_failed
                    },
                    {
                        "manifests_recovered",
                        result.manifests_recovered
                    },
                    {
                        "manifests_unrecovered",
                        result.manifests_unrecovered
                    },
                    {
                        "conflicts_detected",
                        result.conflicts_detected
                    },
                    {
                        "converged",
                        result.converged
                    }
                },
                request
            );
        }

        if (
            target == repair_route
            || target == maintenance_route
        )
        {
            const app::RepairReplicasResult result =
                service_->repair_replicas();

            return make_json_response(
                result.fully_repaired
                    ? beast_http::status::ok
                    : beast_http::status::
                          multi_status,
                {
                    {
                        "operation",
                        target == maintenance_route
                            ? "maintenance"
                            : "repair"
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
                        "local_chunks_recovered",
                        result.local_chunks_recovered
                    },
                    {
                        "remote_replicas_observed",
                        result.remote_replicas_observed
                    },
                    {
                        "remote_replicas_created",
                        result.remote_replicas_created
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
                        "fully_repaired",
                        result.fully_repaired
                    }
                },
                request
            );
        }

        if (target == rebalance_route)
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

            beast_http::status status =
                beast_http::status::ok;

            if (
                result.status ==
                "stale_membership_epoch"
            )
            {
                status =
                    beast_http::status::conflict;
            }
            else if (!result.converged)
            {
                status =
                    beast_http::status::multi_status;
            }

            return make_json_response(
                status,
                {
                    {
                        "operation",
                        "rebalance"
                    },
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
                        "manifests_scanned",
                        result.manifests_scanned
                    },
                    {
                        "unique_chunks_scanned",
                        result.unique_chunks_scanned
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
                },
                request
            );
        }
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
            "invalid_admin_json",
            error.what(),
            request
        );
    }
    catch (const std::invalid_argument& error)
    {
        return make_error_response(
            beast_http::status::bad_request,
            "invalid_admin_request",
            error.what(),
            request
        );
    }
    catch (const std::exception&)
    {
        return make_error_response(
            beast_http::status::
                internal_server_error,
            "admin_operation_failed",
            "The requested administrator operation "
            "could not be completed.",
            request
        );
    }

    return make_error_response(
        beast_http::status::not_found,
        "admin_route_not_found",
        "The requested administrator route "
        "does not exist.",
        request
    );
}

}
