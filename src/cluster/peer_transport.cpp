#include "nexusfs/cluster/peer_transport.hpp"

#include "nexusfs/storage/sha256_hasher.hpp"

#include <boost/asio/error.hpp>
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
#include <cstddef>
#include <cstdint>
#include <limits>
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

std::uint64_t unix_time_milliseconds()
{
    const auto milliseconds =
        std::chrono::duration_cast<
            std::chrono::milliseconds
        >(
            std::chrono::system_clock::now()
                .time_since_epoch()
        ).count();

    if (milliseconds <= 0)
    {
        throw std::runtime_error(
            "System clock produced an invalid timestamp."
        );
    }

    return static_cast<std::uint64_t>(
        milliseconds
    );
}

bool is_lowercase_sha256(
    std::string_view hash
) noexcept
{
    if (hash.size() != 64)
    {
        return false;
    }

    return std::all_of(
        hash.begin(),
        hash.end(),
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
    );
}

void validate_chunk(
    const std::string& chunk_hash,
    const std::vector<std::uint8_t>& data
)
{
    if (!is_lowercase_sha256(chunk_hash))
    {
        throw std::invalid_argument(
            "Remote chunk hash must contain exactly "
            "64 lowercase hexadecimal characters."
        );
    }

    const std::string calculated_hash =
        storage::Sha256Hasher::hash(
            std::span<const std::uint8_t>{
                data.data(),
                data.size()
            }
        );

    if (calculated_hash != chunk_hash)
    {
        throw std::invalid_argument(
            "Remote chunk data does not match "
            "the supplied content hash."
        );
    }
}

beast_http::response<
    beast_http::string_body
>
perform_request(
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

    boost::system::error_code
        address_error;

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

    boost::system::error_code
        shutdown_error;

    stream.socket().shutdown(
        Tcp::socket::shutdown_both,
        shutdown_error
    );

    return response;
}

std::string binary_body(
    const std::vector<std::uint8_t>& data
)
{
    if (data.empty())
    {
        return {};
    }

    return std::string{
        reinterpret_cast<const char*>(
            data.data()
        ),
        data.size()
    };
}

std::vector<std::uint8_t>
binary_data(
    const std::string& body
)
{
    return std::vector<std::uint8_t>{
        body.begin(),
        body.end()
    };
}

std::string placement_score(
    const std::string& chunk_hash,
    const PeerDefinition& peer
)
{
    const std::string input =
        chunk_hash
        + ":"
        + peer.node_id;

    const std::vector<std::uint8_t> bytes{
        input.begin(),
        input.end()
    };

    return storage::Sha256Hasher::hash(
        std::span<const std::uint8_t>{
            bytes.data(),
            bytes.size()
        }
    );
}

}

PeerTransport::PeerTransport(
    std::shared_ptr<
        ClusterNodeFoundation
    > cluster_node,
    std::chrono::milliseconds timeout
)
    : cluster_node_{
          std::move(cluster_node)
      },
      timeout_{
          timeout
      }
{
    if (!cluster_node_)
    {
        throw std::invalid_argument(
            "Peer transport cluster node cannot be null."
        );
    }

    if (timeout_ <= std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument(
            "Peer transport timeout must be positive."
        );
    }
}

void PeerTransport::send_heartbeat(
    const PeerDefinition& peer
)
{
    try
    {
        const std::string request_body =
            ClusterNodeFoundation::
                encode_heartbeat(
                    cluster_node_->
                        local_heartbeat()
                );

        const auto response =
            perform_request(
                *cluster_node_,
                peer,
                beast_http::verb::post,
                "/api/v1/cluster/heartbeat",
                request_body,
                "application/json",
                timeout_
            );

        if (
            response.result() !=
            beast_http::status::ok
        )
        {
            throw std::runtime_error(
                "Peer heartbeat returned HTTP status "
                + std::to_string(
                    response.result_int()
                )
                + "."
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
            || !payload.value(
                "accepted",
                false
            )
            || payload.value(
                "cluster_id",
                std::string{}
            ) !=
                cluster_node_->
                    configuration()
                    .cluster_id
            || payload.value(
                "receiver_node_id",
                std::string{}
            ) != peer.node_id
        )
        {
            throw std::runtime_error(
                "Peer heartbeat acknowledgement "
                "is invalid."
            );
        }

        record_peer_success(
            peer
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

RemoteChunkStoreResult PeerTransport::store_chunk(
    const PeerDefinition& peer,
    const std::string& chunk_hash,
    const std::vector<std::uint8_t>& data
)
{
    validate_chunk(
        chunk_hash,
        data
    );

    try
    {
        const auto response =
            perform_request(
                *cluster_node_,
                peer,
                beast_http::verb::put,
                "/api/v1/cluster/chunks/"
                    + chunk_hash,
                binary_body(
                    data
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
                "Remote chunk publication returned HTTP "
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
                "chunk_hash",
                std::string{}
            ) != chunk_hash
        )
        {
            throw std::runtime_error(
                "Remote chunk acknowledgement is invalid."
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
            return RemoteChunkStoreResult::stored;
        }

        if (result == "already_exists")
        {
            return RemoteChunkStoreResult::
                already_exists;
        }

        throw std::runtime_error(
            "Remote chunk acknowledgement contains "
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
PeerTransport::load_chunk(
    const PeerDefinition& peer,
    const std::string& chunk_hash
)
{
    if (!is_lowercase_sha256(chunk_hash))
    {
        throw std::invalid_argument(
            "Remote chunk hash must contain exactly "
            "64 lowercase hexadecimal characters."
        );
    }

    try
    {
        const auto response =
            perform_request(
                *cluster_node_,
                peer,
                beast_http::verb::get,
                "/api/v1/cluster/chunks/"
                    + chunk_hash,
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
                "Remote chunk load returned HTTP "
                + std::to_string(
                    response.result_int()
                )
                + "."
            );
        }

        const std::vector<std::uint8_t> data =
            binary_data(
                response.body()
            );

        validate_chunk(
            chunk_hash,
            data
        );

        record_peer_success(
            peer
        );

        return data;
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

void PeerTransport::record_peer_success(
    const PeerDefinition& peer
) noexcept
{
    try
    {
        cluster_node_->
            record_peer_heartbeat(
                HeartbeatMessage{
                    cluster_node_->
                        configuration()
                        .cluster_id,
                    peer.node_id,
                    peer.address,
                    peer.port,
                    unix_time_milliseconds()
                }
            );
    }
    catch (...)
    {
    }
}

void PeerTransport::record_peer_failure(
    const PeerDefinition& peer,
    const std::string& error
) noexcept
{
    try
    {
        cluster_node_->
            record_peer_failure(
                peer.node_id,
                error
            );
    }
    catch (...)
    {
    }
}

ReplicationCoordinator::ReplicationCoordinator(
    std::shared_ptr<
        ClusterNodeFoundation
    > cluster_node,
    std::chrono::milliseconds timeout
)
    : cluster_node_{
          std::move(cluster_node)
      },
      transport_{
          cluster_node_,
          timeout
      }
{
    if (!cluster_node_)
    {
        throw std::invalid_argument(
            "Replication coordinator cluster "
            "node cannot be null."
        );
    }
}

ReplicationReport
ReplicationCoordinator::replicate_chunk(
    const std::string& chunk_hash,
    const std::vector<std::uint8_t>& data,
    std::size_t total_replication_factor
)
{
    if (total_replication_factor == 0)
    {
        throw std::invalid_argument(
            "Replication factor must be at least one."
        );
    }

    validate_chunk(
        chunk_hash,
        data
    );

    ReplicationReport report;

    report.requested_remote_replicas =
        total_replication_factor - 1;

    if (report.requested_remote_replicas == 0)
    {
        report.satisfied =
            true;

        return report;
    }

    std::vector<PeerDefinition>
        eligible_peers;

    for (
        const PeerHealthSnapshot& health :
        cluster_node_->peer_health()
    )
    {
        if (
            health.state !=
            PeerHealthState::unavailable
        )
        {
            eligible_peers.push_back(
                health.peer
            );
        }
    }

    const std::vector<PeerDefinition>
        selected =
            select_replica_peers(
                chunk_hash,
                eligible_peers,
                report.requested_remote_replicas
            );

    report.selected_peers =
        selected.size();

    for (
        const PeerDefinition& peer :
        selected
    )
    {
        try
        {
            (void)transport_.store_chunk(
                peer,
                chunk_hash,
                data
            );

            ++report.acknowledged_replicas;

            report.acknowledged_peer_ids.push_back(
                peer.node_id
            );
        }
        catch (const std::exception& error)
        {
            report.failures.push_back(
                ReplicationFailure{
                    peer.node_id,
                    error.what()
                }
            );
        }
    }

    report.satisfied =
        report.acknowledged_replicas
        >= report.requested_remote_replicas;

    return report;
}

std::vector<PeerDefinition>
ReplicationCoordinator::select_replica_peers(
    const std::string& chunk_hash,
    const std::vector<PeerDefinition>& peers,
    std::size_t desired_peer_count
)
{
    if (!is_lowercase_sha256(chunk_hash))
    {
        throw std::invalid_argument(
            "Replica-placement chunk hash is invalid."
        );
    }

    struct ScoredPeer
    {
        PeerDefinition peer;
        std::string score;
    };

    std::vector<ScoredPeer> scored;

    scored.reserve(
        peers.size()
    );

    for (
        const PeerDefinition& peer :
        peers
    )
    {
        scored.push_back(
            ScoredPeer{
                peer,
                placement_score(
                    chunk_hash,
                    peer
                )
            }
        );
    }

    std::sort(
        scored.begin(),
        scored.end(),
        [](
            const ScoredPeer& left,
            const ScoredPeer& right
        )
        {
            if (left.score != right.score)
            {
                return left.score > right.score;
            }

            return (
                left.peer.node_id
                < right.peer.node_id
            );
        }
    );

    const std::size_t selected_count =
        std::min(
            desired_peer_count,
            scored.size()
        );

    std::vector<PeerDefinition> selected;

    selected.reserve(
        selected_count
    );

    for (
        std::size_t index = 0;
        index < selected_count;
        ++index
    )
    {
        selected.push_back(
            scored[index].peer
        );
    }

    return selected;
}

}
