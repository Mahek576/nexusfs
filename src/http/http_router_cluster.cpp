#include "nexusfs/http/http_router.hpp"

#include "nexusfs/storage/chunk_store.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nexusfs::http
{

namespace beast_http =
    boost::beast::http;

namespace
{

constexpr std::string_view cluster_route{
    "/api/v1/cluster"
};

constexpr std::string_view heartbeat_route{
    "/api/v1/cluster/heartbeat"
};

constexpr std::string_view cluster_chunk_prefix{
    "/api/v1/cluster/chunks/"
};

constexpr std::string_view cluster_manifest_prefix{
    "/api/v1/cluster/manifests/"
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

bool is_lowercase_sha256_identifier(
    std::string_view value
) noexcept
{
    return (
        value.size() == 64
        && std::all_of(
            value.begin(),
            value.end(),
            [](char character)
            {
                return (
                    (
                        character >= '0'
                        && character <= '9'
                    )
                    || (
                        character >= 'a'
                        && character <= 'f'
                    )
                );
            }
        )
    );
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

HttpRouter::Response make_binary_response(
    const std::vector<std::uint8_t>& data,
    const std::string& chunk_hash,
    const HttpRouter::Request& request
)
{
    HttpRouter::Response response{
        beast_http::status::ok,
        request.version()
    };

    response.set(
        beast_http::field::server,
        "NexusFS"
    );

    response.set(
        beast_http::field::content_type,
        "application/octet-stream"
    );

    response.set(
        beast_http::field::cache_control,
        "no-store"
    );

    response.set(
        "X-NexusFS-Chunk-Hash",
        chunk_hash
    );

    response.keep_alive(
        request.keep_alive()
    );

    if (data.empty())
    {
        response.body().clear();
    }
    else
    {
        response.body().assign(
            reinterpret_cast<const char*>(
                data.data()
            ),
            data.size()
        );
    }

    response.prepare_payload();

    return response;
}

HttpRouter::Response make_empty_response(
    beast_http::status status,
    const std::string& chunk_hash,
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
        beast_http::field::cache_control,
        "no-store"
    );

    if (!chunk_hash.empty())
    {
        response.set(
            "X-NexusFS-Chunk-Hash",
            chunk_hash
        );
    }

    response.keep_alive(
        request.keep_alive()
    );

    response.body().clear();

    response.content_length(
        0
    );

    return response;
}

HttpRouter::Response make_error_response(
    beast_http::status status,
    std::string code,
    std::string message,
    const HttpRouter::Request& request
)
{
    const nlohmann::ordered_json payload = {
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
    };

    return make_json_response(
        status,
        payload,
        request
    );
}

HttpRouter::Response make_method_response(
    std::string allowed_method,
    const HttpRouter::Request& request
)
{
    HttpRouter::Response response =
        make_error_response(
            beast_http::status::
                method_not_allowed,
            "method_not_allowed",
            "The requested cluster endpoint "
            "does not support this HTTP method.",
            request
        );

    response.set(
        beast_http::field::allow,
        allowed_method
    );

    return response;
}

bool is_configured_peer(
    const cluster::ClusterNodeFoundation& cluster_node,
    std::string_view node_id
)
{
    return std::any_of(
        cluster_node
            .configuration()
            .peers
            .begin(),
        cluster_node
            .configuration()
            .peers
            .end(),
        [node_id](
            const cluster::PeerDefinition& peer
        )
        {
            return peer.node_id == node_id;
        }
    );
}

bool is_authorized_peer_request(
    const HttpRouter::Request& request,
    const cluster::ClusterNodeFoundation& cluster_node
)
{
    const auto supplied_cluster =
        request[cluster_header];

    const auto supplied_node =
        request[node_header];

    return (
        !supplied_cluster.empty()
        && !supplied_node.empty()
        && supplied_cluster ==
            cluster_node
                .configuration()
                .cluster_id
        && is_configured_peer(
            cluster_node,
            std::string_view{
                supplied_node.data(),
                supplied_node.size()
            }
        )
    );
}

nlohmann::ordered_json make_peer_payload(
    const cluster::PeerHealthSnapshot& peer
)
{
    return {
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
            "successful_heartbeats",
            peer.successful_heartbeats
        },
        {
            "consecutive_failures",
            peer.consecutive_failures
        },
        {
            "last_seen_unix_ms",
            peer.last_seen_unix_ms
        },
        {
            "last_error",
            peer.last_error
        }
    };
}

nlohmann::ordered_json make_cluster_payload(
    const cluster::ClusterSnapshot& snapshot
)
{
    nlohmann::ordered_json peers =
        nlohmann::ordered_json::array();

    for (
        const cluster::PeerHealthSnapshot& peer :
        snapshot.peers
    )
    {
        peers.push_back(
            make_peer_payload(
                peer
            )
        );
    }

    return {
        {
            "cluster_id",
            snapshot.configuration.cluster_id
        },
        {
            "local_node",
            {
                {
                    "node_id",
                    snapshot.local_identity.node_id
                },
                {
                    "created_at_unix_ms",
                    snapshot.local_identity
                        .created_at_unix_ms
                },
                {
                    "advertise_address",
                    snapshot.configuration
                        .advertise_address
                },
                {
                    "advertise_port",
                    snapshot.configuration
                        .advertise_port
                }
            }
        },
        {
            "heartbeat",
            {
                {
                    "interval_ms",
                    snapshot.configuration
                        .heartbeat_interval_ms
                },
                {
                    "failure_timeout_ms",
                    snapshot.configuration
                        .failure_timeout_ms
                }
            }
        },
        {
            "summary",
            {
                {
                    "configured_peers",
                    snapshot.peers.size()
                },
                {
                    "healthy",
                    snapshot.healthy_peers
                },
                {
                    "suspect",
                    snapshot.suspect_peers
                },
                {
                    "unavailable",
                    snapshot.unavailable_peers
                },
                {
                    "unknown",
                    snapshot.unknown_peers
                }
            }
        },
        {
            "peers",
            std::move(
                peers
            )
        }
    };
}

HttpRouter::Response handle_chunk_request(
    const HttpRouter::Request& request,
    const std::shared_ptr<
        const app::NexusFsService
    >& service,
    const std::shared_ptr<
        cluster::ClusterNodeFoundation
    >& cluster_node
)
{
    if (
        !is_authorized_peer_request(
            request,
            *cluster_node
        )
    )
    {
        return make_error_response(
            beast_http::status::forbidden,
            "peer_not_authorized",
            "The cluster chunk request did not "
            "provide an authorized peer identity.",
            request
        );
    }

    const std::string_view target =
        request_target(
            request
        );

    const std::string_view hash_view =
        target.substr(
            cluster_chunk_prefix.size()
        );

    if (
        !is_lowercase_sha256_identifier(
            hash_view
        )
    )
    {
        return make_error_response(
            beast_http::status::bad_request,
            "invalid_chunk_hash",
            "The cluster chunk hash must contain "
            "64 lowercase hexadecimal characters.",
            request
        );
    }

    const std::string chunk_hash{
        hash_view
    };

    storage::ChunkStore chunk_store{
        service->storage_root()
    };

    if (
        request.method() ==
        beast_http::verb::put
    )
    {
        try
        {
            const std::vector<std::uint8_t> data{
                request.body().begin(),
                request.body().end()
            };

            const storage::StoreResult result =
                chunk_store.store(
                    storage::FileChunk{
                        0,
                        data,
                        chunk_hash
                    }
                );

            const bool stored =
                result ==
                storage::StoreResult::stored;

            const nlohmann::ordered_json payload = {
                {
                    "chunk_hash",
                    chunk_hash
                },
                {
                    "bytes",
                    data.size()
                },
                {
                    "result",
                    stored
                        ? "stored"
                        : "already_exists"
                }
            };

            return make_json_response(
                stored
                    ? beast_http::status::created
                    : beast_http::status::ok,
                payload,
                request
            );
        }
        catch (const std::invalid_argument& error)
        {
            return make_error_response(
                beast_http::status::bad_request,
                "invalid_chunk",
                error.what(),
                request
            );
        }
        catch (const std::exception&)
        {
            return make_error_response(
                beast_http::status::
                    internal_server_error,
                "chunk_publication_failed",
                "The replicated chunk could not "
                "be published.",
                request
            );
        }
    }

    if (
        request.method() ==
        beast_http::verb::head
    )
    {
        try
        {
            if (
                !chunk_store.contains(
                    chunk_hash
                )
            )
            {
                return make_empty_response(
                    beast_http::status::not_found,
                    {},
                    request
                );
            }

            /*
             * load() verifies the content-addressed object rather
             * than merely checking that a filesystem entry exists.
             */
            (void)chunk_store.load(
                chunk_hash
            );

            return make_empty_response(
                beast_http::status::ok,
                chunk_hash,
                request
            );
        }
        catch (const std::exception&)
        {
            return make_empty_response(
                beast_http::status::
                    internal_server_error,
                {},
                request
            );
        }
    }

    if (
        request.method() ==
        beast_http::verb::get
    )
    {
        try
        {
            if (
                !chunk_store.contains(
                    chunk_hash
                )
            )
            {
                return make_error_response(
                    beast_http::status::not_found,
                    "chunk_not_found",
                    "The requested replicated chunk "
                    "is not stored on this node.",
                    request
                );
            }

            return make_binary_response(
                chunk_store.load(
                    chunk_hash
                ),
                chunk_hash,
                request
            );
        }
        catch (const std::exception&)
        {
            return make_error_response(
                beast_http::status::
                    internal_server_error,
                "chunk_load_failed",
                "The replicated chunk could not "
                "be loaded.",
                request
            );
        }
    }

    return make_method_response(
        "HEAD, GET, PUT",
        request
    );
}

}

HttpRouter::HttpRouter(
    std::shared_ptr<
        const app::NexusFsService
    > service,
    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry,
    std::shared_ptr<
        observability::JsonLogger
    > logger,
    std::shared_ptr<
        cluster::ClusterNodeFoundation
    > cluster_node
)
    : HttpRouter{
          std::move(service),
          std::move(metrics_registry),
          std::move(logger)
      }
{
    if (!cluster_node)
    {
        throw std::invalid_argument(
            "HTTP router cluster foundation "
            "cannot be null."
        );
    }

    cluster_node_ =
        std::move(cluster_node);
}

HttpRouter::Response
HttpRouter::route_cluster_request(
    const Request& request
) const
{
    if (!cluster_node_)
    {
        return make_error_response(
            beast_http::status::
                service_unavailable,
            "cluster_not_configured",
            "Cluster services are not configured "
            "for this NexusFS router.",
            request
        );
    }

    const std::string_view target =
        request_target(
            request
        );

    if (
        target.starts_with(
            cluster_manifest_prefix
        )
    )
    {
        return route_cluster_manifest_request(
            request
        );
    }

    if (
        target.starts_with(
            cluster_chunk_prefix
        )
    )
    {
        return handle_chunk_request(
            request,
            service_,
            cluster_node_
        );
    }

    if (target == cluster_route)
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
                make_cluster_payload(
                    cluster_node_->snapshot()
                ),
                request
            );
        }
        catch (const std::exception&)
        {
            return make_error_response(
                beast_http::status::
                    internal_server_error,
                "cluster_snapshot_failed",
                "The cluster status snapshot "
                "could not be generated.",
                request
            );
        }
    }

    if (target == heartbeat_route)
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

        try
        {
            const cluster::HeartbeatMessage heartbeat =
                cluster::ClusterNodeFoundation::
                    decode_heartbeat(
                        request.body()
                    );

            cluster_node_->record_peer_heartbeat(
                heartbeat
            );

            logger_->log(
                observability::LogLevel::debug,
                "cluster_heartbeat_received",
                "Cluster peer heartbeat received.",
                {
                    observability::LogField{
                        "peer_node_id",
                        heartbeat.node_id
                    },
                    observability::LogField{
                        "peer_address",
                        heartbeat
                            .advertise_address
                    },
                    observability::LogField{
                        "peer_port",
                        static_cast<std::uint64_t>(
                            heartbeat.advertise_port
                        )
                    }
                }
            );

            const nlohmann::ordered_json payload = {
                {
                    "accepted",
                    true
                },
                {
                    "cluster_id",
                    cluster_node_
                        ->configuration()
                        .cluster_id
                },
                {
                    "receiver_node_id",
                    cluster_node_
                        ->identity()
                        .node_id
                }
            };

            return make_json_response(
                beast_http::status::ok,
                payload,
                request
            );
        }
        catch (const std::invalid_argument& error)
        {
            return make_error_response(
                beast_http::status::
                    bad_request,
                "invalid_heartbeat",
                error.what(),
                request
            );
        }
        catch (const std::exception&)
        {
            return make_error_response(
                beast_http::status::
                    internal_server_error,
                "heartbeat_processing_failed",
                "The peer heartbeat could not "
                "be processed.",
                request
            );
        }
    }

    return make_error_response(
        beast_http::status::not_found,
        "route_not_found",
        "The requested cluster route "
        "does not exist.",
        request
    );
}

}
