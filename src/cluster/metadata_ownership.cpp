#include "nexusfs/cluster/metadata_ownership.hpp"

#include "nexusfs/storage/sha256_hasher.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nexusfs::cluster
{

namespace
{

bool is_lowercase_sha256(
    std::string_view identifier
) noexcept
{
    return (
        identifier.size() == 64
        && std::all_of(
            identifier.begin(),
            identifier.end(),
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

void validate_candidate(
    const MetadataOwner& candidate
)
{
    if (candidate.node_id.empty())
    {
        throw std::invalid_argument(
            "Metadata owner node ID cannot be empty."
        );
    }

    if (candidate.address.empty())
    {
        throw std::invalid_argument(
            "Metadata owner address cannot be empty."
        );
    }

    if (candidate.port == 0)
    {
        throw std::invalid_argument(
            "Metadata owner port cannot be zero."
        );
    }
}

std::string ownership_score(
    const std::string& manifest_id,
    const MetadataOwner& candidate
)
{
    const std::string input =
        manifest_id
        + ":metadata:"
        + candidate.node_id;

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

MetadataOwner MetadataOwnership::select_owner(
    const std::string& manifest_id,
    const NodeIdentity& local_identity,
    const ClusterConfiguration& configuration
)
{
    const std::vector<MetadataOwner> owners =
        ordered_owners(
            manifest_id,
            local_identity,
            configuration
        );

    if (owners.empty())
    {
        throw std::runtime_error(
            "Metadata ownership produced no candidates."
        );
    }

    return owners.front();
}

std::vector<MetadataOwner>
MetadataOwnership::ordered_owners(
    const std::string& manifest_id,
    const NodeIdentity& local_identity,
    const ClusterConfiguration& configuration
)
{
    if (!is_lowercase_sha256(manifest_id))
    {
        throw std::invalid_argument(
            "Manifest ID must contain exactly "
            "64 lowercase hexadecimal characters."
        );
    }

    MetadataOwner local_owner{
        local_identity.node_id,
        configuration.advertise_address,
        configuration.advertise_port,
        true
    };

    validate_candidate(
        local_owner
    );

    std::vector<MetadataOwner> candidates;

    candidates.reserve(
        configuration.peers.size()
        + 1
    );

    candidates.push_back(
        std::move(local_owner)
    );

    std::unordered_set<std::string>
        node_ids;

    node_ids.reserve(
        configuration.peers.size()
        + 1
    );

    node_ids.insert(
        local_identity.node_id
    );

    for (
        const PeerDefinition& peer :
        configuration.peers
    )
    {
        MetadataOwner candidate{
            peer.node_id,
            peer.address,
            peer.port,
            false
        };

        validate_candidate(
            candidate
        );

        if (
            !node_ids.insert(
                candidate.node_id
            ).second
        )
        {
            throw std::invalid_argument(
                "Metadata ownership candidates contain "
                "a duplicate node ID: "
                + candidate.node_id
            );
        }

        candidates.push_back(
            std::move(candidate)
        );
    }

    struct ScoredOwner
    {
        MetadataOwner owner;
        std::string score;
    };

    std::vector<ScoredOwner> scored;

    scored.reserve(
        candidates.size()
    );

    for (
        MetadataOwner& candidate :
        candidates
    )
    {
        /*
         * Calculate the rendezvous score before moving the owner.
         * Scoring a moved-from candidate can discard its node ID
         * and collapse placement into deterministic tie-breaking.
         */
        std::string score =
            ownership_score(
                manifest_id,
                candidate
            );

        scored.push_back(
            ScoredOwner{
                std::move(candidate),
                std::move(score)
            }
        );
    }

    std::sort(
        scored.begin(),
        scored.end(),
        [](
            const ScoredOwner& left,
            const ScoredOwner& right
        )
        {
            if (left.score != right.score)
            {
                return left.score > right.score;
            }

            return (
                left.owner.node_id
                < right.owner.node_id
            );
        }
    );

    std::vector<MetadataOwner> ordered;

    ordered.reserve(
        scored.size()
    );

    for (
        ScoredOwner& candidate :
        scored
    )
    {
        ordered.push_back(
            std::move(candidate.owner)
        );
    }

    return ordered;
}

}
