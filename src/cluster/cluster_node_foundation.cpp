#include "nexusfs/cluster/cluster_node_foundation.hpp"

#include "nexusfs/storage/durable_file.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nexusfs::cluster
{

namespace
{

constexpr std::uint64_t minimum_heartbeat_interval_ms =
    100;

constexpr std::size_t node_id_length =
    32;

bool is_ascii_identifier_character(
    char character
) noexcept
{
    return (
        (
            character >= 'a'
            && character <= 'z'
        )
        || (
            character >= 'A'
            && character <= 'Z'
        )
        || (
            character >= '0'
            && character <= '9'
        )
        || character == '-'
        || character == '_'
        || character == '.'
    );
}

void validate_cluster_id(
    std::string_view cluster_id
)
{
    if (
        cluster_id.empty()
        || cluster_id.size() > 64
        || !std::all_of(
            cluster_id.begin(),
            cluster_id.end(),
            is_ascii_identifier_character
        )
    )
    {
        throw std::invalid_argument(
            "Cluster ID must contain between 1 and 64 "
            "ASCII letters, digits, '.', '_' or '-'."
        );
    }
}

bool is_lowercase_hexadecimal(
    std::string_view value
) noexcept
{
    if (value.size() != node_id_length)
    {
        return false;
    }

    return std::all_of(
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
    );
}

void validate_node_id(
    std::string_view node_id
)
{
    if (!is_lowercase_hexadecimal(node_id))
    {
        throw std::invalid_argument(
            "Node ID must contain exactly 32 lowercase "
            "hexadecimal characters."
        );
    }
}

void validate_address(
    std::string_view address
)
{
    if (
        address.empty()
        || address.find_first_of(
            " \t\r\n"
        ) != std::string_view::npos
    )
    {
        throw std::invalid_argument(
            "Cluster address cannot be empty or "
            "contain whitespace."
        );
    }

    boost::system::error_code error;

    (void)boost::asio::ip::make_address(
        std::string{
            address
        },
        error
    );

    if (error)
    {
        throw std::invalid_argument(
            "Cluster address must be a numeric "
            "IPv4 or IPv6 address."
        );
    }
}

void validate_port(
    std::uint16_t port
)
{
    if (port == 0)
    {
        throw std::invalid_argument(
            "Cluster port must be between 1 and 65535."
        );
    }
}

std::uint64_t unix_time_milliseconds()
{
    const auto now =
        std::chrono::system_clock::now();

    const auto milliseconds =
        std::chrono::duration_cast<
            std::chrono::milliseconds
        >(
            now.time_since_epoch()
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

std::string generate_node_id()
{
    std::array<std::uint8_t, 16> bytes{};

    std::random_device random_device;

    for (std::uint8_t& byte : bytes)
    {
        byte =
            static_cast<std::uint8_t>(
                random_device()
                & 0xffU
            );
    }

    const std::uint64_t timestamp =
        unix_time_milliseconds();

    const std::size_t thread_token =
        std::hash<std::thread::id>{}(
            std::this_thread::get_id()
        );

    for (
        std::size_t index = 0;
        index < bytes.size();
        ++index
    )
    {
        const std::uint8_t timestamp_byte =
            static_cast<std::uint8_t>(
                (
                    timestamp
                    >> (
                        (index % 8U)
                        * 8U
                    )
                )
                & 0xffU
            );

        const std::uint8_t thread_byte =
            static_cast<std::uint8_t>(
                (
                    thread_token
                    >> (
                        (index % sizeof(std::size_t))
                        * 8U
                    )
                )
                & 0xffU
            );

        bytes[index] ^=
            timestamp_byte
            ^ thread_byte;
    }

    constexpr char hexadecimal_digits[] =
        "0123456789abcdef";

    std::string node_id;
    node_id.reserve(
        node_id_length
    );

    for (const std::uint8_t byte : bytes)
    {
        node_id.push_back(
            hexadecimal_digits[
                byte >> 4U
            ]
        );

        node_id.push_back(
            hexadecimal_digits[
                byte & 0x0fU
            ]
        );
    }

    validate_node_id(
        node_id
    );

    return node_id;
}

std::filesystem::path make_temporary_path(
    const std::filesystem::path& destination
)
{
    static std::atomic<std::uint64_t>
        sequence{
            0
        };

    std::filesystem::path temporary =
        destination;

    temporary +=
        ".tmp."
        + std::to_string(
            unix_time_milliseconds()
        )
        + "."
        + std::to_string(
            std::hash<std::thread::id>{}(
                std::this_thread::get_id()
            )
        )
        + "."
        + std::to_string(
            sequence.fetch_add(
                1,
                std::memory_order_relaxed
            )
        );

    return temporary;
}

nlohmann::json load_json_file(
    const std::filesystem::path& path
)
{
    std::ifstream input{
        path,
        std::ios::binary
    };

    if (!input.is_open())
    {
        throw std::runtime_error(
            "Failed to open cluster metadata file: "
            + path.string()
        );
    }

    nlohmann::json payload;

    try
    {
        input >> payload;
    }
    catch (const nlohmann::json::exception& error)
    {
        throw std::runtime_error(
            "Failed to parse cluster metadata "
            + path.string()
            + ": "
            + error.what()
        );
    }

    return payload;
}

void publish_json_no_replace(
    const std::filesystem::path& destination,
    const nlohmann::ordered_json& payload
)
{
    const std::filesystem::path temporary =
        make_temporary_path(
            destination
        );

    try
    {
        {
            std::ofstream output{
                temporary,
                std::ios::binary
                    | std::ios::trunc
            };

            if (!output.is_open())
            {
                throw std::runtime_error(
                    "Failed to create temporary cluster "
                    "metadata file: "
                    + temporary.string()
                );
            }

            output
                << payload.dump(2)
                << '\n';

            output.close();

            if (!output)
            {
                throw std::runtime_error(
                    "Failed to write cluster metadata file: "
                    + temporary.string()
                );
            }
        }

        const storage::DurablePublishResult result =
            storage::publish_file_durably(
                temporary,
                destination
            );

        if (
            result ==
            storage::DurablePublishResult::
                destination_exists
        )
        {
            std::error_code cleanup_error;

            std::filesystem::remove(
                temporary,
                cleanup_error
            );
        }
    }
    catch (...)
    {
        std::error_code cleanup_error;

        std::filesystem::remove(
            temporary,
            cleanup_error
        );

        throw;
    }
}

NodeIdentity load_or_create_identity(
    const std::filesystem::path& identity_path
)
{
    std::error_code existence_error;

    const bool exists =
        std::filesystem::exists(
            identity_path,
            existence_error
        );

    if (existence_error)
    {
        throw std::runtime_error(
            "Failed to inspect node identity path: "
            + existence_error.message()
        );
    }

    if (!exists)
    {
        const NodeIdentity candidate{
            generate_node_id(),
            unix_time_milliseconds()
        };

        const nlohmann::ordered_json payload = {
            {
                "schema_version",
                1
            },
            {
                "node_id",
                candidate.node_id
            },
            {
                "created_at_unix_ms",
                candidate.created_at_unix_ms
            }
        };

        publish_json_no_replace(
            identity_path,
            payload
        );
    }

    const nlohmann::json payload =
        load_json_file(
            identity_path
        );

    if (
        !payload.is_object()
        || payload.value(
            "schema_version",
            0
        ) != 1
        || !payload.contains("node_id")
        || !payload.contains(
            "created_at_unix_ms"
        )
    )
    {
        throw std::runtime_error(
            "Node identity metadata has an "
            "unsupported schema."
        );
    }

    NodeIdentity identity{
        payload.at("node_id")
            .get<std::string>(),
        payload.at("created_at_unix_ms")
            .get<std::uint64_t>()
    };

    validate_node_id(
        identity.node_id
    );

    if (identity.created_at_unix_ms == 0)
    {
        throw std::runtime_error(
            "Node identity timestamp must be positive."
        );
    }

    return identity;
}

void validate_configuration(
    const ClusterConfiguration& configuration,
    std::string_view local_node_id
)
{
    validate_cluster_id(
        configuration.cluster_id
    );

    validate_address(
        configuration.advertise_address
    );

    validate_port(
        configuration.advertise_port
    );

    if (
        configuration.heartbeat_interval_ms
        < minimum_heartbeat_interval_ms
    )
    {
        throw std::invalid_argument(
            "Heartbeat interval must be at least "
            "100 milliseconds."
        );
    }

    if (
        configuration.failure_timeout_ms
        <= configuration.heartbeat_interval_ms
    )
    {
        throw std::invalid_argument(
            "Failure timeout must be greater than "
            "the heartbeat interval."
        );
    }

    if (configuration.replication_factor == 0)
    {
        throw std::invalid_argument(
            "Replication factor must be at least one."
        );
    }

    if (
        configuration.replica_maintenance_interval_ms
        < 100
    )
    {
        throw std::invalid_argument(
            "Replica-maintenance interval must be "
            "at least 100 milliseconds."
        );
    }

    std::unordered_set<std::string>
        node_ids;

    std::unordered_set<std::string>
        endpoints;

    for (
        const PeerDefinition& peer :
        configuration.peers
    )
    {
        validate_node_id(
            peer.node_id
        );

        validate_address(
            peer.address
        );

        validate_port(
            peer.port
        );

        if (peer.node_id == local_node_id)
        {
            throw std::invalid_argument(
                "Cluster peers cannot contain the "
                "local node ID."
            );
        }

        if (
            !node_ids.insert(
                peer.node_id
            ).second
        )
        {
            throw std::invalid_argument(
                "Cluster peer node IDs must be unique."
            );
        }

        const std::string endpoint =
            peer.address
            + ":"
            + std::to_string(
                peer.port
            );

        if (
            !endpoints.insert(
                endpoint
            ).second
        )
        {
            throw std::invalid_argument(
                "Cluster peer endpoints must be unique."
            );
        }
    }
}

ClusterConfiguration load_or_create_configuration(
    const std::filesystem::path& configuration_path,
    const NodeIdentity& identity,
    const std::string& advertise_address,
    std::uint16_t advertise_port
)
{
    std::error_code existence_error;

    const bool exists =
        std::filesystem::exists(
            configuration_path,
            existence_error
        );

    if (existence_error)
    {
        throw std::runtime_error(
            "Failed to inspect cluster configuration path: "
            + existence_error.message()
        );
    }

    if (!exists)
    {
        const ClusterConfiguration candidate{
            "standalone-"
                + identity.node_id.substr(
                    0,
                    8
                ),
            advertise_address,
            advertise_port,
            1000,
            5000,
            1,
            true,
            {}
        };

        const nlohmann::ordered_json payload = {
            {
                "schema_version",
                1
            },
            {
                "cluster_id",
                candidate.cluster_id
            },
            {
                "advertise_address",
                candidate.advertise_address
            },
            {
                "advertise_port",
                candidate.advertise_port
            },
            {
                "heartbeat_interval_ms",
                candidate.heartbeat_interval_ms
            },
            {
                "failure_timeout_ms",
                candidate.failure_timeout_ms
            },
            {
                "replication_factor",
                candidate.replication_factor
            },
            {
                "strict_replication",
                candidate.strict_replication
            },
            {
                "replica_maintenance_interval_ms",
                candidate
                    .replica_maintenance_interval_ms
            },
            {
                "peers",
                nlohmann::ordered_json::array()
            }
        };

        publish_json_no_replace(
            configuration_path,
            payload
        );
    }

    const nlohmann::json payload =
        load_json_file(
            configuration_path
        );

    if (
        !payload.is_object()
        || payload.value(
            "schema_version",
            0
        ) != 1
    )
    {
        throw std::runtime_error(
            "Cluster configuration has an "
            "unsupported schema."
        );
    }

    ClusterConfiguration configuration;

    configuration.cluster_id =
        payload.at("cluster_id")
            .get<std::string>();

    configuration.advertise_address =
        payload.at("advertise_address")
            .get<std::string>();

    const std::uint64_t advertise_port_value =
        payload.at("advertise_port")
            .get<std::uint64_t>();

    if (
        advertise_port_value == 0
        || advertise_port_value >
            std::numeric_limits<
                std::uint16_t
            >::max()
    )
    {
        throw std::runtime_error(
            "Cluster advertise port is invalid."
        );
    }

    configuration.advertise_port =
        static_cast<std::uint16_t>(
            advertise_port_value
        );

    configuration.heartbeat_interval_ms =
        payload.at("heartbeat_interval_ms")
            .get<std::uint64_t>();

    configuration.failure_timeout_ms =
        payload.at("failure_timeout_ms")
            .get<std::uint64_t>();

    const std::uint64_t replication_factor_value =
        payload.value(
            "replication_factor",
            static_cast<std::uint64_t>(1)
        );

    if (
        replication_factor_value == 0
        || replication_factor_value >
            static_cast<std::uint64_t>(
                std::numeric_limits<
                    std::size_t
                >::max()
            )
    )
    {
        throw std::runtime_error(
            "Cluster replication factor is invalid."
        );
    }

    configuration.replication_factor =
        static_cast<std::size_t>(
            replication_factor_value
        );

    configuration.strict_replication =
        payload.value(
            "strict_replication",
            true
        );

    configuration.replica_maintenance_interval_ms =
        payload.value(
            "replica_maintenance_interval_ms",
            static_cast<std::uint64_t>(
                30000
            )
        );

    const nlohmann::json& peers =
        payload.at("peers");

    if (!peers.is_array())
    {
        throw std::runtime_error(
            "Cluster peers must be a JSON array."
        );
    }

    for (const nlohmann::json& peer : peers)
    {
        const std::uint64_t port_value =
            peer.at("port")
                .get<std::uint64_t>();

        if (
            port_value == 0
            || port_value >
                std::numeric_limits<
                    std::uint16_t
                >::max()
        )
        {
            throw std::runtime_error(
                "Cluster peer port is invalid."
            );
        }

        configuration.peers.push_back(
            PeerDefinition{
                peer.at("node_id")
                    .get<std::string>(),
                peer.at("address")
                    .get<std::string>(),
                static_cast<std::uint16_t>(
                    port_value
                )
            }
        );
    }

    validate_configuration(
        configuration,
        identity.node_id
    );

    return configuration;
}

struct PeerRuntime
{
    PeerDefinition definition;
    PeerHealthState state{
        PeerHealthState::unknown
    };

    std::uint64_t successful_heartbeats{0};
    std::uint64_t consecutive_failures{0};
    std::uint64_t last_seen_unix_ms{0};
    std::string last_error;

    bool has_last_seen{false};
    ClusterNodeFoundation::Clock::time_point
        last_seen;
};

PeerHealthState calculate_state(
    const PeerRuntime& peer,
    const ClusterConfiguration& configuration,
    ClusterNodeFoundation::Clock::time_point now
)
{
    if (!peer.has_last_seen)
    {
        return (
            peer.consecutive_failures >= 3
            ? PeerHealthState::unavailable
            : (
                peer.consecutive_failures > 0
                ? PeerHealthState::suspect
                : PeerHealthState::unknown
            )
        );
    }

    const auto elapsed =
        std::chrono::duration_cast<
            std::chrono::milliseconds
        >(
            now - peer.last_seen
        );

    const std::uint64_t elapsed_ms =
        elapsed.count() > 0
        ? static_cast<std::uint64_t>(
              elapsed.count()
          )
        : 0;

    if (
        elapsed_ms >=
        configuration.failure_timeout_ms
    )
    {
        return PeerHealthState::unavailable;
    }

    if (
        peer.consecutive_failures > 0
        || elapsed_ms >=
            configuration.failure_timeout_ms
            / 2U
    )
    {
        return PeerHealthState::suspect;
    }

    return PeerHealthState::healthy;
}

}

struct ClusterNodeFoundation::State
{
    std::filesystem::path cluster_directory;
    NodeIdentity identity;
    ClusterConfiguration configuration;

    mutable std::mutex mutex;

    std::unordered_map<
        std::string,
        PeerRuntime
    > peers;
};

ClusterNodeFoundation::ClusterNodeFoundation(
    std::unique_ptr<State> state
)
    : state_{
          std::move(state)
      }
{
}

ClusterNodeFoundation::~ClusterNodeFoundation() =
    default;

std::shared_ptr<ClusterNodeFoundation>
ClusterNodeFoundation::load_or_create(
    const std::filesystem::path& storage_root,
    std::string advertise_address,
    std::uint16_t advertise_port
)
{
    if (storage_root.empty())
    {
        throw std::invalid_argument(
            "Cluster storage root cannot be empty."
        );
    }

    validate_address(
        advertise_address
    );

    validate_port(
        advertise_port
    );

    const std::filesystem::path
        cluster_directory =
            storage_root
            / "cluster";

    std::error_code directory_error;

    std::filesystem::create_directories(
        cluster_directory,
        directory_error
    );

    if (directory_error)
    {
        throw std::runtime_error(
            "Failed to create cluster metadata directory: "
            + directory_error.message()
        );
    }

    auto state =
        std::make_unique<State>();

    state->cluster_directory =
        cluster_directory;

    state->identity =
        load_or_create_identity(
            cluster_directory
            / "node_identity.json"
        );

    state->configuration =
        load_or_create_configuration(
            cluster_directory
            / "cluster.json",
            state->identity,
            advertise_address,
            advertise_port
        );

    for (
        const PeerDefinition& peer :
        state->configuration.peers
    )
    {
        state->peers.emplace(
            peer.node_id,
            PeerRuntime{
                peer
            }
        );
    }

    return std::shared_ptr<
        ClusterNodeFoundation
    >(
        new ClusterNodeFoundation{
            std::move(state)
        }
    );
}

const NodeIdentity&
ClusterNodeFoundation::identity() const noexcept
{
    return state_->identity;
}

const ClusterConfiguration&
ClusterNodeFoundation::configuration() const noexcept
{
    return state_->configuration;
}

HeartbeatMessage
ClusterNodeFoundation::local_heartbeat() const
{
    return HeartbeatMessage{
        state_->configuration.cluster_id,
        state_->identity.node_id,
        state_->configuration.advertise_address,
        state_->configuration.advertise_port,
        unix_time_milliseconds()
    };
}

void ClusterNodeFoundation::record_peer_heartbeat(
    const HeartbeatMessage& heartbeat,
    Clock::time_point observed_at
)
{
    validate_cluster_id(
        heartbeat.cluster_id
    );

    validate_node_id(
        heartbeat.node_id
    );

    validate_address(
        heartbeat.advertise_address
    );

    validate_port(
        heartbeat.advertise_port
    );

    if (
        heartbeat.cluster_id !=
        state_->configuration.cluster_id
    )
    {
        throw std::invalid_argument(
            "Heartbeat belongs to a different cluster."
        );
    }

    const std::lock_guard lock{
        state_->mutex
    };

    const auto position =
        state_->peers.find(
            heartbeat.node_id
        );

    if (position == state_->peers.end())
    {
        throw std::invalid_argument(
            "Heartbeat sender is not a configured peer."
        );
    }

    PeerRuntime& peer =
        position->second;

    if (
        heartbeat.advertise_address !=
            peer.definition.address
        || heartbeat.advertise_port !=
            peer.definition.port
    )
    {
        throw std::invalid_argument(
            "Heartbeat endpoint does not match "
            "the configured peer endpoint."
        );
    }

    peer.state =
        PeerHealthState::healthy;

    ++peer.successful_heartbeats;

    peer.consecutive_failures =
        0;

    peer.last_seen_unix_ms =
        unix_time_milliseconds();

    peer.last_error.clear();

    peer.has_last_seen =
        true;

    peer.last_seen =
        observed_at;
}

void ClusterNodeFoundation::record_peer_failure(
    std::string_view peer_node_id,
    std::string error_message
)
{
    validate_node_id(
        peer_node_id
    );

    const std::lock_guard lock{
        state_->mutex
    };

    const auto position =
        state_->peers.find(
            std::string{
                peer_node_id
            }
        );

    if (position == state_->peers.end())
    {
        throw std::invalid_argument(
            "Cannot record failure for an "
            "unknown peer."
        );
    }

    PeerRuntime& peer =
        position->second;

    ++peer.consecutive_failures;

    peer.last_error =
        std::move(
            error_message
        );

    peer.state =
        peer.consecutive_failures >= 3
        ? PeerHealthState::unavailable
        : PeerHealthState::suspect;
}

std::vector<PeerHealthSnapshot>
ClusterNodeFoundation::peer_health(
    Clock::time_point now
) const
{
    const std::lock_guard lock{
        state_->mutex
    };

    std::vector<PeerHealthSnapshot>
        snapshots;

    snapshots.reserve(
        state_->peers.size()
    );

    for (
        const auto& [
            node_id,
            peer
        ] :
        state_->peers
    )
    {
        (void)node_id;

        snapshots.push_back(
            PeerHealthSnapshot{
                peer.definition,
                calculate_state(
                    peer,
                    state_->configuration,
                    now
                ),
                peer.successful_heartbeats,
                peer.consecutive_failures,
                peer.last_seen_unix_ms,
                peer.last_error
            }
        );
    }

    std::sort(
        snapshots.begin(),
        snapshots.end(),
        [](
            const PeerHealthSnapshot& left,
            const PeerHealthSnapshot& right
        )
        {
            return (
                left.peer.node_id
                < right.peer.node_id
            );
        }
    );

    return snapshots;
}

ClusterSnapshot ClusterNodeFoundation::snapshot(
    Clock::time_point now
) const
{
    ClusterSnapshot result;

    result.local_identity =
        state_->identity;

    result.configuration =
        state_->configuration;

    result.peers =
        peer_health(
            now
        );

    for (
        const PeerHealthSnapshot& peer :
        result.peers
    )
    {
        switch (peer.state)
        {
            case PeerHealthState::healthy:
                ++result.healthy_peers;
                break;

            case PeerHealthState::suspect:
                ++result.suspect_peers;
                break;

            case PeerHealthState::unavailable:
                ++result.unavailable_peers;
                break;

            case PeerHealthState::unknown:
                ++result.unknown_peers;
                break;
        }
    }

    return result;
}

std::string ClusterNodeFoundation::encode_heartbeat(
    const HeartbeatMessage& heartbeat
)
{
    validate_cluster_id(
        heartbeat.cluster_id
    );

    validate_node_id(
        heartbeat.node_id
    );

    validate_address(
        heartbeat.advertise_address
    );

    validate_port(
        heartbeat.advertise_port
    );

    if (heartbeat.sent_at_unix_ms == 0)
    {
        throw std::invalid_argument(
            "Heartbeat timestamp must be positive."
        );
    }

    const nlohmann::ordered_json payload = {
        {
            "schema_version",
            1
        },
        {
            "cluster_id",
            heartbeat.cluster_id
        },
        {
            "node_id",
            heartbeat.node_id
        },
        {
            "advertise_address",
            heartbeat.advertise_address
        },
        {
            "advertise_port",
            heartbeat.advertise_port
        },
        {
            "sent_at_unix_ms",
            heartbeat.sent_at_unix_ms
        }
    };

    return payload.dump();
}

HeartbeatMessage ClusterNodeFoundation::decode_heartbeat(
    std::string_view encoded_heartbeat
)
{
    const nlohmann::json payload =
        nlohmann::json::parse(
            encoded_heartbeat,
            nullptr,
            false
        );

    if (
        payload.is_discarded()
        || !payload.is_object()
        || payload.value(
            "schema_version",
            0
        ) != 1
    )
    {
        throw std::invalid_argument(
            "Heartbeat JSON is invalid or uses "
            "an unsupported schema."
        );
    }

    const std::uint64_t port_value =
        payload.at("advertise_port")
            .get<std::uint64_t>();

    if (
        port_value == 0
        || port_value >
            std::numeric_limits<
                std::uint16_t
            >::max()
    )
    {
        throw std::invalid_argument(
            "Heartbeat advertise port is invalid."
        );
    }

    HeartbeatMessage heartbeat{
        payload.at("cluster_id")
            .get<std::string>(),
        payload.at("node_id")
            .get<std::string>(),
        payload.at("advertise_address")
            .get<std::string>(),
        static_cast<std::uint16_t>(
            port_value
        ),
        payload.at("sent_at_unix_ms")
            .get<std::uint64_t>()
    };

    validate_cluster_id(
        heartbeat.cluster_id
    );

    validate_node_id(
        heartbeat.node_id
    );

    validate_address(
        heartbeat.advertise_address
    );

    validate_port(
        heartbeat.advertise_port
    );

    if (heartbeat.sent_at_unix_ms == 0)
    {
        throw std::invalid_argument(
            "Heartbeat timestamp must be positive."
        );
    }

    return heartbeat;
}

const std::filesystem::path&
ClusterNodeFoundation::cluster_directory()
    const noexcept
{
    return state_->cluster_directory;
}

std::string_view peer_health_state_name(
    PeerHealthState state
) noexcept
{
    switch (state)
    {
        case PeerHealthState::unknown:
            return "unknown";

        case PeerHealthState::healthy:
            return "healthy";

        case PeerHealthState::suspect:
            return "suspect";

        case PeerHealthState::unavailable:
            return "unavailable";
    }

    return "unknown";
}

}
