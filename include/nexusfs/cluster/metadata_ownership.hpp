#ifndef NEXUSFS_CLUSTER_METADATA_OWNERSHIP_HPP
#define NEXUSFS_CLUSTER_METADATA_OWNERSHIP_HPP

#include "nexusfs/cluster/cluster_node_foundation.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nexusfs::cluster
{

struct MetadataOwner
{
    std::string node_id;
    std::string address;
    std::uint16_t port{0};
    bool local{false};
};

/*
 * Deterministic rendezvous-hash metadata placement.
 *
 * Every manifest is assigned one primary metadata owner from the
 * local node plus all configured peers. ordered_owners() returns the
 * same deterministic ordering and is used for remote fallback.
 */
class MetadataOwnership final
{
public:
    [[nodiscard]] static MetadataOwner
    select_owner(
        const std::string& manifest_id,
        const NodeIdentity& local_identity,
        const ClusterConfiguration& configuration
    );

    [[nodiscard]] static std::vector<
        MetadataOwner
    > ordered_owners(
        const std::string& manifest_id,
        const NodeIdentity& local_identity,
        const ClusterConfiguration& configuration
    );
};

}

#endif
