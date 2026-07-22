#include "nexusfs/cluster/metadata_ownership.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

void require_true(
    bool condition,
    const std::string& test_name
)
{
    if (!condition)
    {
        throw std::runtime_error(
            test_name + " failed."
        );
    }
}

template <
    typename Actual,
    typename Expected
>
void require_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& test_name
)
{
    if (actual != expected)
    {
        throw std::runtime_error(
            test_name + " failed."
        );
    }
}

template <typename Operation>
void require_exception(
    Operation&& operation,
    const std::string& test_name
)
{
    bool exception_thrown =
        false;

    try
    {
        operation();
    }
    catch (const std::exception&)
    {
        exception_thrown =
            true;
    }

    require_true(
        exception_thrown,
        test_name
    );
}

std::string manifest_id_for(
    const std::string& value
)
{
    const std::vector<std::uint8_t> bytes{
        value.begin(),
        value.end()
    };

    return nexusfs::storage::
        Sha256Hasher::hash(
            std::span<const std::uint8_t>{
                bytes.data(),
                bytes.size()
            }
        );
}

nexusfs::cluster::NodeIdentity
make_local_identity()
{
    return {
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        1
    };
}

nexusfs::cluster::ClusterConfiguration
make_configuration()
{
    nexusfs::cluster::ClusterConfiguration
        configuration;

    configuration.cluster_id =
        "metadata-ownership-test";

    configuration.advertise_address =
        "127.0.0.1";

    configuration.advertise_port =
        9100;

    configuration.peers = {
        {
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            "127.0.0.1",
            9101
        },
        {
            "cccccccccccccccccccccccccccccccc",
            "127.0.0.1",
            9102
        },
        {
            "dddddddddddddddddddddddddddddddd",
            "127.0.0.1",
            9103
        }
    };

    return configuration;
}

void test_deterministic_owner_selection()
{
    const auto identity =
        make_local_identity();

    const auto configuration =
        make_configuration();

    const std::string manifest_id =
        manifest_id_for(
            "deterministic-owner"
        );

    const auto first =
        nexusfs::cluster::
            MetadataOwnership::select_owner(
                manifest_id,
                identity,
                configuration
            );

    const auto second =
        nexusfs::cluster::
            MetadataOwnership::select_owner(
                manifest_id,
                identity,
                configuration
            );

    require_equal(
        first.node_id,
        second.node_id,
        "Deterministic metadata-owner node test"
    );

    require_equal(
        first.address,
        second.address,
        "Deterministic metadata-owner address test"
    );

    require_equal(
        first.port,
        second.port,
        "Deterministic metadata-owner port test"
    );

    require_equal(
        first.local,
        second.local,
        "Deterministic metadata-owner locality test"
    );
}

void test_peer_order_independence()
{
    const auto identity =
        make_local_identity();

    auto first_configuration =
        make_configuration();

    auto second_configuration =
        make_configuration();

    std::reverse(
        second_configuration.peers.begin(),
        second_configuration.peers.end()
    );

    const std::string manifest_id =
        manifest_id_for(
            "peer-order-independence"
        );

    const auto first_order =
        nexusfs::cluster::
            MetadataOwnership::ordered_owners(
                manifest_id,
                identity,
                first_configuration
            );

    const auto second_order =
        nexusfs::cluster::
            MetadataOwnership::ordered_owners(
                manifest_id,
                identity,
                second_configuration
            );

    require_equal(
        first_order.size(),
        second_order.size(),
        "Metadata-owner ordering size test"
    );

    for (
        std::size_t index = 0;
        index < first_order.size();
        ++index
    )
    {
        require_equal(
            first_order[index].node_id,
            second_order[index].node_id,
            "Metadata-owner peer-order independence test"
        );
    }
}

void test_all_nodes_participate_once()
{
    const auto identity =
        make_local_identity();

    const auto configuration =
        make_configuration();

    const auto owners =
        nexusfs::cluster::
            MetadataOwnership::ordered_owners(
                manifest_id_for(
                    "all-owner-candidates"
                ),
                identity,
                configuration
            );

    require_equal(
        owners.size(),
        static_cast<std::size_t>(4),
        "Metadata-owner candidate count test"
    );

    std::unordered_set<std::string>
        unique_node_ids;

    std::size_t local_count =
        0;

    for (const auto& owner : owners)
    {
        unique_node_ids.insert(
            owner.node_id
        );

        if (owner.local)
        {
            ++local_count;

            require_equal(
                owner.node_id,
                identity.node_id,
                "Local metadata-owner identity test"
            );
        }
    }

    require_equal(
        unique_node_ids.size(),
        owners.size(),
        "Metadata-owner uniqueness test"
    );

    require_equal(
        local_count,
        static_cast<std::size_t>(1),
        "Local metadata-owner participation test"
    );
}

void test_primary_matches_order_head()
{
    const auto identity =
        make_local_identity();

    const auto configuration =
        make_configuration();

    const std::string manifest_id =
        manifest_id_for(
            "primary-order-head"
        );

    const auto ordered =
        nexusfs::cluster::
            MetadataOwnership::ordered_owners(
                manifest_id,
                identity,
                configuration
            );

    const auto owner =
        nexusfs::cluster::
            MetadataOwnership::select_owner(
                manifest_id,
                identity,
                configuration
            );

    require_equal(
        owner.node_id,
        ordered.front().node_id,
        "Primary metadata-owner ordering test"
    );
}

void test_invalid_inputs()
{
    const auto identity =
        make_local_identity();

    const auto configuration =
        make_configuration();

    require_exception(
        [
            &identity,
            &configuration
        ]()
        {
            (void)nexusfs::cluster::
                MetadataOwnership::select_owner(
                    "not-a-manifest-id",
                    identity,
                    configuration
                );
        },
        "Invalid manifest identifier test"
    );

    auto duplicate_configuration =
        configuration;

    duplicate_configuration.peers.push_back(
        {
            identity.node_id,
            "127.0.0.1",
            9199
        }
    );

    require_exception(
        [
            &identity,
            &duplicate_configuration
        ]()
        {
            (void)nexusfs::cluster::
                MetadataOwnership::ordered_owners(
                    manifest_id_for(
                        "duplicate-owner"
                    ),
                    identity,
                    duplicate_configuration
                );
        },
        "Duplicate metadata-owner node test"
    );
}

}

int main()
{
    try
    {
        test_deterministic_owner_selection();

        std::cout
            << "[PASS] Deterministic metadata ownership\n";

        test_peer_order_independence();

        std::cout
            << "[PASS] Metadata ownership peer-order independence\n";

        test_all_nodes_participate_once();

        std::cout
            << "[PASS] Metadata ownership candidate participation\n";

        test_primary_matches_order_head();

        std::cout
            << "[PASS] Primary metadata owner ordering\n";

        test_invalid_inputs();

        std::cout
            << "[PASS] Metadata ownership validation\n";

        std::cout
            << "All NexusFS metadata ownership tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "[FAIL] "
            << error.what()
            << '\n';

        return 1;
    }
}
