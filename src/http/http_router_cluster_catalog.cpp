#include "nexusfs/http/http_router.hpp"

#include "nexusfs/cluster/metadata_catalog.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
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

constexpr std::string_view cluster_header{
    "X-NexusFS-Cluster-ID"
};

constexpr std::string_view node_header{
    "X-NexusFS-Node-ID"
};

constexpr std::string_view catalog_digest_header{
    "X-NexusFS-Catalog-Digest"
};

constexpr std::string_view cluster_catalog_route{
    "/api/v1/cluster/catalog"
};

constexpr std::string_view cluster_catalog_sync_route{
    "/api/v1/cluster/catalog/sync"
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

bool is_configured_peer(
    const cluster::ClusterNodeFoundation& cluster_node,
    std::string_view node_id
)
{
    return cluster_node.is_known_peer(
        node_id
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

}

HttpRouter::Response
HttpRouter::route_cluster_catalog_request(
    const Request& request
) const
{
    if (
        !cluster_node_
        || !is_authorized_peer_request(
            request,
            *cluster_node_
        )
    )
    {
        return make_error_response(
            beast_http::status::forbidden,
            "peer_not_authorized",
            "The cluster catalog request did not provide "
            "an authorized peer identity.",
            request
        );
    }

    const std::string_view target =
        request_target(
            request
        );

    if (
        target ==
        cluster_catalog_sync_route
    )
    {
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
                    "The cluster catalog synchronization "
                    "endpoint supports only POST.",
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
            const app::SynchronizeMetadataCatalogResult
                result =
                    service_->
                        synchronize_metadata_catalog();

            nlohmann::ordered_json files =
                nlohmann::ordered_json::array();

            for (
                const app::StoredFileSummary& file :
                result.files
            )
            {
                files.push_back(
                    {
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
                        }
                    }
                );
            }

            const nlohmann::ordered_json payload = {
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
                    "remote_entries_observed",
                    result.remote_entries_observed
                },
                {
                    "unique_entries_discovered",
                    result.unique_entries_discovered
                },
                {
                    "manifests_already_local",
                    result.manifests_already_local
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
                },
                {
                    "files",
                    std::move(files)
                }
            };

            HttpRouter::Response response{
                result.converged
                    ? beast_http::status::ok
                    : beast_http::status::multi_status,
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
        catch (const std::exception&)
        {
            return make_error_response(
                beast_http::status::
                    internal_server_error,
                "catalog_synchronization_failed",
                "The cluster metadata catalog could not "
                "be synchronized.",
                request
            );
        }
    }

    if (
        target !=
        cluster_catalog_route
    )
    {
        return make_error_response(
            beast_http::status::not_found,
            "catalog_route_not_found",
            "The requested cluster catalog route "
            "does not exist.",
            request
        );
    }

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
                "The cluster catalog endpoint supports "
                "only GET.",
                request
            );

        response.set(
            beast_http::field::allow,
            "GET"
        );

        return response;
    }

    try
    {
        const app::ListFilesResult local_catalog =
            service_->list_files();

        std::vector<
            cluster::MetadataCatalogEntry
        > entries;

        entries.reserve(
            local_catalog.files.size()
        );

        for (
            const app::StoredFileSummary& file :
            local_catalog.files
        )
        {
            entries.push_back(
                cluster::MetadataCatalogEntry{
                    file.manifest_id,
                    file.original_filename,
                    file.file_size,
                    static_cast<std::uint64_t>(
                        file.configured_chunk_size
                    ),
                    static_cast<std::uint64_t>(
                        file.chunk_count
                    )
                }
            );
        }

        const cluster::MetadataCatalogSnapshot snapshot =
            cluster::MetadataCatalogCodec::create(
                cluster_node_
                    ->identity()
                    .node_id,
                std::move(entries)
            );

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
            "application/json"
        );

        response.set(
            beast_http::field::cache_control,
            "no-store"
        );

        response.set(
            catalog_digest_header,
            snapshot.digest
        );

        response.keep_alive(
            request.keep_alive()
        );

        response.body() =
            cluster::MetadataCatalogCodec::encode(
                snapshot
            );

        response.prepare_payload();

        return response;
    }
    catch (const std::exception&)
    {
        return make_error_response(
            beast_http::status::
                internal_server_error,
            "catalog_generation_failed",
            "The local metadata catalog could not "
            "be generated.",
            request
        );
    }
}

}
