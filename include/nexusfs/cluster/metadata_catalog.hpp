#ifndef NEXUSFS_CLUSTER_METADATA_CATALOG_HPP
#define NEXUSFS_CLUSTER_METADATA_CATALOG_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace nexusfs::cluster
{

struct MetadataCatalogEntry
{
    std::string manifest_id;
    std::string original_filename;

    std::uint64_t file_size{0};
    std::uint64_t chunk_size{0};
    std::uint64_t chunk_count{0};

    [[nodiscard]] bool operator==(
        const MetadataCatalogEntry&
    ) const = default;
};

struct MetadataCatalogSnapshot
{
    std::string node_id;
    std::string digest;

    std::vector<MetadataCatalogEntry>
        entries;
};

/*
 * Canonical, digest-protected metadata catalog encoding.
 *
 * Entries are strictly sorted by manifest ID and duplicates are
 * rejected. The digest covers the schema version, node identity and
 * every catalog field.
 */
class MetadataCatalogCodec final
{
public:
    [[nodiscard]] static MetadataCatalogSnapshot
    create(
        std::string node_id,
        std::vector<MetadataCatalogEntry> entries
    );

    [[nodiscard]] static std::string encode(
        const MetadataCatalogSnapshot& snapshot
    );

    [[nodiscard]] static MetadataCatalogSnapshot decode(
        const std::string& payload,
        const std::string& expected_node_id
    );

    [[nodiscard]] static bool entry_matches_manifest(
        const MetadataCatalogEntry& entry,
        const std::vector<std::uint8_t>& encoded_manifest
    );
};

}

#endif
