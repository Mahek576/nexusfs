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
