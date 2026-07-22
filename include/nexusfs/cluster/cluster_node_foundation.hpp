#ifndef NEXUSFS_CLUSTER_CLUSTER_NODE_FOUNDATION_HPP
#define NEXUSFS_CLUSTER_CLUSTER_NODE_FOUNDATION_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nexusfs::cluster
{

struct NodeIdentity
{
    std::string node_id;
    std::uint64_t created_at_unix_ms{0};
};

struct PeerDefinition
{
    std::string node_id;
    std::string address;
    std::uint16_t port{0};
};

struct ClusterConfiguration
{
    std::string cluster_id;
    std::string advertise_address;
    std::uint16_t advertise_port{0};

    std::uint64_t heartbeat_interval_ms{
        1000
    };

    std::uint64_t failure_timeout_ms{
        5000
    };

    std::vector<PeerDefinition> peers;
};

enum class PeerHealthState
{
    unknown,
    healthy,
    suspect,
    unavailable
};

[[nodiscard]] std::string_view
peer_health_state_name(
    PeerHealthState state
) noexcept;

struct PeerHealthSnapshot
{
    PeerDefinition peer;
    PeerHealthState state{
        PeerHealthState::unknown
    };

    std::uint64_t successful_heartbeats{0};
    std::uint64_t consecutive_failures{0};
    std::uint64_t last_seen_unix_ms{0};
    std::string last_error;
};

struct HeartbeatMessage
{
    std::string cluster_id;
    std::string node_id;
    std::string advertise_address;
    std::uint16_t advertise_port{0};
    std::uint64_t sent_at_unix_ms{0};
};

struct ClusterSnapshot
{
    NodeIdentity local_identity;
    ClusterConfiguration configuration;
    std::vector<PeerHealthSnapshot> peers;

    std::uint64_t healthy_peers{0};
    std::uint64_t suspect_peers{0};
    std::uint64_t unavailable_peers{0};
    std::uint64_t unknown_peers{0};
};

/*
 * Persistent local node identity plus thread-safe peer health state.
 *
 * Files:
 *
 *   <storage-root>/cluster/node_identity.json
 *   <storage-root>/cluster/cluster.json
 *
 * Both files are published through the durable no-replace storage
 * primitive. Existing identity is never silently regenerated.
 */
class ClusterNodeFoundation final
{
public:
    using Clock =
        std::chrono::steady_clock;

    static std::shared_ptr<
        ClusterNodeFoundation
    > load_or_create(
        const std::filesystem::path& storage_root,
        std::string advertise_address,
        std::uint16_t advertise_port
    );

    ~ClusterNodeFoundation();

    ClusterNodeFoundation(
        const ClusterNodeFoundation&
    ) = delete;

    ClusterNodeFoundation& operator=(
        const ClusterNodeFoundation&
    ) = delete;

    [[nodiscard]] const NodeIdentity&
    identity() const noexcept;

    [[nodiscard]] const ClusterConfiguration&
    configuration() const noexcept;

    [[nodiscard]] HeartbeatMessage
    local_heartbeat() const;

    void record_peer_heartbeat(
        const HeartbeatMessage& heartbeat,
        Clock::time_point observed_at =
            Clock::now()
    );

    void record_peer_failure(
        std::string_view peer_node_id,
        std::string error_message
    );

    [[nodiscard]] std::vector<
        PeerHealthSnapshot
    > peer_health(
        Clock::time_point now =
            Clock::now()
    ) const;

    [[nodiscard]] ClusterSnapshot snapshot(
        Clock::time_point now =
            Clock::now()
    ) const;

    [[nodiscard]] static std::string
    encode_heartbeat(
        const HeartbeatMessage& heartbeat
    );

    [[nodiscard]] static HeartbeatMessage
    decode_heartbeat(
        std::string_view encoded_heartbeat
    );

    [[nodiscard]] const std::filesystem::path&
    cluster_directory() const noexcept;

private:
    struct State;

    explicit ClusterNodeFoundation(
        std::unique_ptr<State> state
    );

    std::unique_ptr<State> state_;
};

}

#endif
