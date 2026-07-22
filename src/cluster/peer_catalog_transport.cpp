#include "nexusfs/cluster/peer_transport.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

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

constexpr std::string_view catalog_digest_header{
    "X-NexusFS-Catalog-Digest"
};

beast_http::response<
    beast_http::string_body
>
perform_catalog_request(
    const ClusterNodeFoundation& cluster_node,
    const PeerDefinition& peer,
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
        beast_http::empty_body
    > request{
        beast_http::verb::get,
        "/api/v1/cluster/catalog",
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

    request.keep_alive(
        false
    );

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

MetadataCatalogSnapshot PeerTransport::load_catalog(
    const PeerDefinition& peer
)
{
    try
    {
        const auto response =
            perform_catalog_request(
                *cluster_node_,
                peer,
                timeout_
            );

        if (
            response.result() !=
            beast_http::status::ok
        )
        {
            throw std::runtime_error(
                "Remote metadata catalog returned HTTP "
                + std::to_string(
                    response.result_int()
                )
                + "."
            );
        }

        const MetadataCatalogSnapshot snapshot =
            MetadataCatalogCodec::decode(
                response.body(),
                peer.node_id
            );

        const auto supplied_digest =
            response[
                catalog_digest_header
            ];

        if (
            supplied_digest.empty()
            || std::string_view{
                   supplied_digest.data(),
                   supplied_digest.size()
               } != snapshot.digest
        )
        {
            throw std::runtime_error(
                "Remote catalog digest header is invalid."
            );
        }

        record_peer_success(
            peer
        );

        return snapshot;
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
