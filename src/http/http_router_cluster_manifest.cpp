#include "nexusfs/http/http_router.hpp"

#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/manifest_store.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
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

constexpr std::string_view cluster_manifest_prefix{
    "/api/v1/cluster/manifests/"
};

constexpr std::string_view cluster_header{
    "X-NexusFS-Cluster-ID"
};

constexpr std::string_view node_header{
    "X-NexusFS-Node-ID"
};

constexpr std::string_view manifest_id_header{
    "X-NexusFS-Manifest-ID"
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

void validate_canonical_manifest(
    const std::string& manifest_id,
    const std::vector<std::uint8_t>& encoded_manifest
)
{
    if (encoded_manifest.empty())
    {
        throw std::invalid_argument(
            "Encoded manifest cannot be empty."
        );
    }

    const std::string calculated_id =
        storage::Sha256Hasher::hash(
            std::span<const std::uint8_t>{
                encoded_manifest.data(),
                encoded_manifest.size()
            }
        );

    if (calculated_id != manifest_id)
    {
        throw std::invalid_argument(
            "Encoded manifest does not match "
            "the requested manifest ID."
        );
    }

    try
    {
        const storage::FileManifest decoded =
            storage::FileManifestCodec::decode(
                std::span<const std::uint8_t>{
                    encoded_manifest.data(),
                    encoded_manifest.size()
                }
            );

        const std::vector<std::uint8_t> canonical =
            storage::FileManifestCodec::encode(
                decoded
            );

        if (canonical != encoded_manifest)
        {
            throw std::invalid_argument(
                "Manifest is not canonically encoded."
            );
        }
    }
    catch (const std::invalid_argument&)
    {
        throw;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(
            std::string{
                "Manifest decoding failed: "
            }
            + error.what()
        );
    }
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

HttpRouter::Response make_binary_response(
    const std::vector<std::uint8_t>& encoded_manifest,
    const std::string& manifest_id,
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
        manifest_id_header,
        manifest_id
    );

    response.keep_alive(
        request.keep_alive()
    );

    response.body().assign(
        reinterpret_cast<const char*>(
            encoded_manifest.data()
        ),
        encoded_manifest.size()
    );

    response.prepare_payload();

    return response;
}

HttpRouter::Response make_empty_response(
    beast_http::status status,
    const std::string& manifest_id,
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

    if (!manifest_id.empty())
    {
        response.set(
            manifest_id_header,
            manifest_id
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

}

HttpRouter::Response
HttpRouter::route_cluster_manifest_request(
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
            "The cluster manifest request did not "
            "provide an authorized peer identity.",
            request
        );
    }

    const std::string_view target =
        request_target(
            request
        );

    const std::string_view manifest_id_view =
        target.substr(
            cluster_manifest_prefix.size()
        );

    if (
        !is_lowercase_sha256_identifier(
            manifest_id_view
        )
    )
    {
        return make_error_response(
            beast_http::status::bad_request,
            "invalid_manifest_id",
            "The cluster manifest ID must contain "
            "64 lowercase hexadecimal characters.",
            request
        );
    }

    const std::string manifest_id{
        manifest_id_view
    };

    storage::ManifestStore manifest_store{
        service_->storage_root()
    };

    if (
        request.method() ==
        beast_http::verb::put
    )
    {
        const std::vector<std::uint8_t>
            encoded_manifest{
                request.body().begin(),
                request.body().end()
            };

        try
        {
            validate_canonical_manifest(
                manifest_id,
                encoded_manifest
            );
        }
        catch (const std::exception& error)
        {
            return make_error_response(
                beast_http::status::bad_request,
                "invalid_manifest",
                error.what(),
                request
            );
        }

        try
        {
            const storage::ManifestStoreResult result =
                manifest_store.store(
                    manifest_id,
                    encoded_manifest
                );

            const bool stored =
                result ==
                storage::ManifestStoreResult::stored;

            return make_json_response(
                stored
                    ? beast_http::status::created
                    : beast_http::status::ok,
                {
                    {
                        "manifest_id",
                        manifest_id
                    },
                    {
                        "bytes",
                        encoded_manifest.size()
                    },
                    {
                        "result",
                        stored
                            ? "stored"
                            : "already_exists"
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
                "manifest_publication_failed",
                "The replicated manifest could not "
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
                !manifest_store.contains(
                    manifest_id
                )
            )
            {
                return make_empty_response(
                    beast_http::status::not_found,
                    {},
                    request
                );
            }

            const std::vector<std::uint8_t>
                encoded_manifest =
                    manifest_store.load(
                        manifest_id
                    );

            validate_canonical_manifest(
                manifest_id,
                encoded_manifest
            );

            return make_empty_response(
                beast_http::status::ok,
                manifest_id,
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
                !manifest_store.contains(
                    manifest_id
                )
            )
            {
                return make_error_response(
                    beast_http::status::not_found,
                    "manifest_not_found",
                    "The requested replicated manifest "
                    "is not stored on this node.",
                    request
                );
            }

            const std::vector<std::uint8_t>
                encoded_manifest =
                    manifest_store.load(
                        manifest_id
                    );

            validate_canonical_manifest(
                manifest_id,
                encoded_manifest
            );

            return make_binary_response(
                encoded_manifest,
                manifest_id,
                request
            );
        }
        catch (const std::exception&)
        {
            return make_error_response(
                beast_http::status::
                    internal_server_error,
                "manifest_load_failed",
                "The replicated manifest could not "
                "be loaded.",
                request
            );
        }
    }

    HttpRouter::Response response =
        make_error_response(
            beast_http::status::
                method_not_allowed,
            "method_not_allowed",
            "The cluster manifest endpoint supports "
            "only HEAD, GET and PUT.",
            request
        );

    response.set(
        beast_http::field::allow,
        "HEAD, GET, PUT"
    );

    return response;
}

}
