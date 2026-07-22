#include "nexusfs/cluster/peer_transport.hpp"

#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nexusfs::cluster
{

namespace
{

namespace asio =
    boost::asio;

namespace beast =
    boost::beast;

namespace beast_http =
    boost::beast::http;

using Tcp =
    asio::ip::tcp;

constexpr std::string_view cluster_header{
    "X-NexusFS-Cluster-ID"
};

constexpr std::string_view node_header{
    "X-NexusFS-Node-ID"
};

constexpr std::string_view manifest_id_header{
    "X-NexusFS-Manifest-ID"
};

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

void validate_encoded_manifest(
    const std::string& manifest_id,
    const std::vector<std::uint8_t>& encoded_manifest
)
{
    if (!is_lowercase_sha256_identifier(manifest_id))
    {
        throw std::invalid_argument(
            "Remote manifest ID must contain exactly "
            "64 lowercase hexadecimal characters."
        );
    }

    if (encoded_manifest.empty())
    {
        throw std::invalid_argument(
            "Remote encoded manifest cannot be empty."
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
            "Remote manifest bytes do not match "
            "the supplied manifest ID."
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
                "Remote manifest is not canonically encoded."
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
                "Remote manifest decoding failed: "
            }
            + error.what()
        );
    }
}

std::string binary_body(
    const std::vector<std::uint8_t>& data
)
{
    return std::string{
        reinterpret_cast<const char*>(
            data.data()
        ),
        data.size()
    };
}

std::vector<std::uint8_t> binary_data(
    const std::string& body
)
{
    return std::vector<std::uint8_t>{
        body.begin(),
        body.end()
    };
}

beast_http::response<
    beast_http::string_body
>
perform_manifest_request(
    const ClusterNodeFoundation& cluster_node,
    const PeerDefinition& peer,
    beast_http::verb method,
    std::string target,
    std::string body,
    std::string content_type,
    std::chrono::milliseconds timeout
)
{
    asio::io_context context{
        1
    };

    boost::system::error_code address_error;

    const asio::ip::address address =
        asio::ip::make_address(
            peer.address,
            address_error
        );

    if (address_error)
    {
        throw std::runtime_error(
            "Peer address is invalid: "
            + address_error.message()
        );
    }

    beast::tcp_stream stream{
        context
    };

    stream.expires_after(
        timeout
    );

    stream.connect(
        Tcp::endpoint{
            address,
            peer.port
        }
    );

    beast_http::request<
        beast_http::string_body
    > request{
        method,
        std::move(target),
        11
    };

    request.set(
        beast_http::field::host,
        peer.address
    );

    request.set(
        beast_http::field::user_agent,
        "NexusFS-PeerTransport/1"
    );

    request.set(
        cluster_header,
        cluster_node
            .configuration()
            .cluster_id
    );

    request.set(
        node_header,
        cluster_node
            .identity()
            .node_id
    );

    if (!content_type.empty())
    {
        request.set(
            beast_http::field::content_type,
            content_type
        );
    }

    request.keep_alive(
        false
    );

    request.body() =
        std::move(body);

    request.prepare_payload();

    stream.expires_after(
        timeout
    );

    beast_http::write(
        stream,
        request
    );

    beast::flat_buffer buffer;

    beast_http::response<
        beast_http::string_body
    > response;

    stream.expires_after(
        timeout
    );

    beast_http::read(
        stream,
        buffer,
        response
    );

    boost::system::error_code shutdown_error;

    stream.socket().shutdown(
        Tcp::socket::shutdown_both,
        shutdown_error
    );

    return response;
}

}

bool PeerTransport::manifest_exists(
    const PeerDefinition& peer,
    const std::string& manifest_id
)
{
    if (!is_lowercase_sha256_identifier(manifest_id))
    {
        throw std::invalid_argument(
            "Remote manifest ID must contain exactly "
            "64 lowercase hexadecimal characters."
        );
    }

    try
    {
        const auto response =
            perform_manifest_request(
                *cluster_node_,
                peer,
                beast_http::verb::head,
                "/api/v1/cluster/manifests/"
                    + manifest_id,
                {},
                {},
                timeout_
            );

        if (
            response.result() ==
            beast_http::status::not_found
        )
        {
            record_peer_success(
                peer
            );

            return false;
        }

        if (
            response.result() !=
            beast_http::status::ok
        )
        {
            throw std::runtime_error(
                "Remote manifest probe returned HTTP "
                + std::to_string(
                    response.result_int()
                )
                + "."
            );
        }

        const auto supplied_id =
            response[manifest_id_header];

        if (
            supplied_id.empty()
            || std::string_view{
                   supplied_id.data(),
                   supplied_id.size()
               } != manifest_id
        )
        {
            throw std::runtime_error(
                "Remote manifest probe acknowledgement "
                "is invalid."
            );
        }

        record_peer_success(
            peer
        );

        return true;
    }
    catch (const std::exception& error)
    {
        record_peer_failure(
            peer,
            error.what()
        );

        throw;
    }
}

RemoteManifestStoreResult
PeerTransport::store_manifest(
    const PeerDefinition& peer,
    const std::string& manifest_id,
    const std::vector<std::uint8_t>& encoded_manifest
)
{
    validate_encoded_manifest(
        manifest_id,
        encoded_manifest
    );

    try
    {
        const auto response =
            perform_manifest_request(
                *cluster_node_,
                peer,
                beast_http::verb::put,
                "/api/v1/cluster/manifests/"
                    + manifest_id,
                binary_body(
                    encoded_manifest
                ),
                "application/octet-stream",
                timeout_
            );

        if (
            response.result() !=
                beast_http::status::created
            && response.result() !=
                beast_http::status::ok
        )
        {
            throw std::runtime_error(
                "Remote manifest publication returned HTTP "
                + std::to_string(
                    response.result_int()
                )
                + ": "
                + response.body()
            );
        }

        const nlohmann::json payload =
            nlohmann::json::parse(
                response.body(),
                nullptr,
                false
            );

        if (
            payload.is_discarded()
            || !payload.is_object()
            || payload.value(
                "manifest_id",
                std::string{}
            ) != manifest_id
        )
        {
            throw std::runtime_error(
                "Remote manifest acknowledgement is invalid."
            );
        }

        const std::string result =
            payload.value(
                "result",
                std::string{}
            );

        record_peer_success(
            peer
        );

        if (result == "stored")
        {
            return RemoteManifestStoreResult::stored;
        }

        if (result == "already_exists")
        {
            return RemoteManifestStoreResult::
                already_exists;
        }

        throw std::runtime_error(
            "Remote manifest acknowledgement contains "
            "an unknown result."
        );
    }
    catch (const std::exception& error)
    {
        record_peer_failure(
            peer,
            error.what()
        );

        throw;
    }
}

std::vector<std::uint8_t>
PeerTransport::load_manifest(
    const PeerDefinition& peer,
    const std::string& manifest_id
)
{
    if (!is_lowercase_sha256_identifier(manifest_id))
    {
        throw std::invalid_argument(
            "Remote manifest ID must contain exactly "
            "64 lowercase hexadecimal characters."
        );
    }

    try
    {
        const auto response =
            perform_manifest_request(
                *cluster_node_,
                peer,
                beast_http::verb::get,
                "/api/v1/cluster/manifests/"
                    + manifest_id,
                {},
                {},
                timeout_
            );

        if (
            response.result() !=
            beast_http::status::ok
        )
        {
            throw std::runtime_error(
                "Remote manifest load returned HTTP "
                + std::to_string(
                    response.result_int()
                )
                + "."
            );
        }

        const auto supplied_id =
            response[manifest_id_header];

        if (
            supplied_id.empty()
            || std::string_view{
                   supplied_id.data(),
                   supplied_id.size()
               } != manifest_id
        )
        {
            throw std::runtime_error(
                "Remote manifest response header is invalid."
            );
        }

        const std::vector<std::uint8_t> encoded_manifest =
            binary_data(
                response.body()
            );

        validate_encoded_manifest(
            manifest_id,
            encoded_manifest
        );

        record_peer_success(
            peer
        );

        return encoded_manifest;
    }
    catch (const std::exception& error)
    {
        record_peer_failure(
            peer,
            error.what()
        );

        throw;
    }
}

}
