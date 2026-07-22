#include "nexusfs/cluster/metadata_catalog.hpp"

#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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

constexpr std::uint64_t schema_version{
    1
};

bool is_lowercase_hex_identifier(
    std::string_view value,
    std::size_t expected_length
) noexcept
{
    return (
        value.size() == expected_length
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

void validate_entry(
    const MetadataCatalogEntry& entry
)
{
    if (
        !is_lowercase_hex_identifier(
            entry.manifest_id,
            64
        )
    )
    {
        throw std::invalid_argument(
            "Catalog manifest ID must contain exactly "
            "64 lowercase hexadecimal characters."
        );
    }

    if (entry.original_filename.empty())
    {
        throw std::invalid_argument(
            "Catalog filename cannot be empty."
        );
    }

    if (entry.chunk_size == 0)
    {
        throw std::invalid_argument(
            "Catalog chunk size must be greater than zero."
        );
    }

    const std::uint64_t expected_chunk_count =
        entry.file_size == 0
        ? 0
        : (
            (
                entry.file_size
                + entry.chunk_size
                - 1
            )
            / entry.chunk_size
        );

    if (
        entry.chunk_count !=
        expected_chunk_count
    )
    {
        throw std::invalid_argument(
            "Catalog chunk count is inconsistent "
            "with file and chunk sizes."
        );
    }
}

nlohmann::ordered_json entry_payload(
    const MetadataCatalogEntry& entry
)
{
    return {
        {
            "manifest_id",
            entry.manifest_id
        },
        {
            "original_filename",
            entry.original_filename
        },
        {
            "file_size",
            entry.file_size
        },
        {
            "chunk_size",
            entry.chunk_size
        },
        {
            "chunk_count",
            entry.chunk_count
        }
    };
}

nlohmann::ordered_json unsigned_payload(
    const std::string& node_id,
    const std::vector<MetadataCatalogEntry>& entries
)
{
    nlohmann::ordered_json encoded_entries =
        nlohmann::ordered_json::array();

    for (
        const MetadataCatalogEntry& entry :
        entries
    )
    {
        encoded_entries.push_back(
            entry_payload(
                entry
            )
        );
    }

    return {
        {
            "schema_version",
            schema_version
        },
        {
            "node_id",
            node_id
        },
        {
            "entries",
            std::move(
                encoded_entries
            )
        }
    };
}

std::string calculate_digest(
    const std::string& node_id,
    const std::vector<MetadataCatalogEntry>& entries
)
{
    const std::string canonical =
        unsigned_payload(
            node_id,
            entries
        ).dump();

    const std::vector<std::uint8_t> bytes{
        canonical.begin(),
        canonical.end()
    };

    return storage::Sha256Hasher::hash(
        std::span<const std::uint8_t>{
            bytes.data(),
            bytes.size()
        }
    );
}

MetadataCatalogEntry decode_entry(
    const nlohmann::json& payload
)
{
    if (!payload.is_object())
    {
        throw std::invalid_argument(
            "Catalog entry must be a JSON object."
        );
    }

    MetadataCatalogEntry entry{
        payload.at(
            "manifest_id"
        ).get<std::string>(),
        payload.at(
            "original_filename"
        ).get<std::string>(),
        payload.at(
            "file_size"
        ).get<std::uint64_t>(),
        payload.at(
            "chunk_size"
        ).get<std::uint64_t>(),
        payload.at(
            "chunk_count"
        ).get<std::uint64_t>()
    };

    validate_entry(
        entry
    );

    return entry;
}

}

MetadataCatalogSnapshot
MetadataCatalogCodec::create(
    std::string node_id,
    std::vector<MetadataCatalogEntry> entries
)
{
    if (
        !is_lowercase_hex_identifier(
            node_id,
            32
        )
    )
    {
        throw std::invalid_argument(
            "Catalog node ID must contain exactly "
            "32 lowercase hexadecimal characters."
        );
    }

    for (
        const MetadataCatalogEntry& entry :
        entries
    )
    {
        validate_entry(
            entry
        );
    }

    std::sort(
        entries.begin(),
        entries.end(),
        [](
            const MetadataCatalogEntry& left,
            const MetadataCatalogEntry& right
        )
        {
            return (
                left.manifest_id
                < right.manifest_id
            );
        }
    );

    for (
        std::size_t index = 1;
        index < entries.size();
        ++index
    )
    {
        if (
            entries[index - 1].manifest_id ==
            entries[index].manifest_id
        )
        {
            throw std::invalid_argument(
                "Catalog contains a duplicate manifest ID: "
                + entries[index].manifest_id
            );
        }
    }

    const std::string digest =
        calculate_digest(
            node_id,
            entries
        );

    return MetadataCatalogSnapshot{
        std::move(node_id),
        digest,
        std::move(entries)
    };
}

std::string MetadataCatalogCodec::encode(
    const MetadataCatalogSnapshot& snapshot
)
{
    const MetadataCatalogSnapshot canonical =
        create(
            snapshot.node_id,
            snapshot.entries
        );

    if (
        canonical.digest !=
        snapshot.digest
    )
    {
        throw std::invalid_argument(
            "Catalog snapshot digest is invalid."
        );
    }

    nlohmann::ordered_json payload =
        unsigned_payload(
            canonical.node_id,
            canonical.entries
        );

    payload[
        "digest"
    ] = canonical.digest;

    return payload.dump();
}

MetadataCatalogSnapshot
MetadataCatalogCodec::decode(
    const std::string& payload,
    const std::string& expected_node_id
)
{
    try
    {
        const nlohmann::json decoded =
            nlohmann::json::parse(
                payload
            );

        if (
            !decoded.is_object()
            || decoded.at(
                "schema_version"
            ).get<std::uint64_t>() !=
                schema_version
        )
        {
            throw std::invalid_argument(
                "Unsupported catalog schema version."
            );
        }

        const std::string node_id =
            decoded.at(
                "node_id"
            ).get<std::string>();

        if (
            node_id !=
            expected_node_id
        )
        {
            throw std::invalid_argument(
                "Catalog node identity does not match "
                "the requested peer."
            );
        }

        const nlohmann::json& entries_payload =
            decoded.at(
                "entries"
            );

        if (!entries_payload.is_array())
        {
            throw std::invalid_argument(
                "Catalog entries must be an array."
            );
        }

        std::vector<MetadataCatalogEntry>
            decoded_entries;

        decoded_entries.reserve(
            entries_payload.size()
        );

        for (
            const nlohmann::json& entry :
            entries_payload
        )
        {
            decoded_entries.push_back(
                decode_entry(
                    entry
                )
            );
        }

        const MetadataCatalogSnapshot canonical =
            create(
                node_id,
                decoded_entries
            );

        /*
         * Reject valid-but-noncanonical ordering rather than silently
         * normalizing it.
         */
        if (
            canonical.entries !=
            decoded_entries
        )
        {
            throw std::invalid_argument(
                "Catalog entries are not canonically ordered."
            );
        }

        const std::string supplied_digest =
            decoded.at(
                "digest"
            ).get<std::string>();

        if (
            supplied_digest !=
            canonical.digest
        )
        {
            throw std::invalid_argument(
                "Catalog digest validation failed."
            );
        }

        return canonical;
    }
    catch (const std::invalid_argument&)
    {
        throw;
    }
    catch (const std::exception& error)
    {
        throw std::invalid_argument(
            std::string{
                "Catalog decoding failed: "
            }
            + error.what()
        );
    }
}

bool MetadataCatalogCodec::entry_matches_manifest(
    const MetadataCatalogEntry& entry,
    const std::vector<std::uint8_t>& encoded_manifest
)
{
    try
    {
        validate_entry(
            entry
        );

        const storage::FileManifest manifest =
            storage::FileManifestCodec::decode(
                encoded_manifest
            );

        const std::vector<std::uint8_t> canonical =
            storage::FileManifestCodec::encode(
                manifest
            );

        if (
            canonical !=
            encoded_manifest
        )
        {
            return false;
        }

        const std::string calculated_id =
            storage::Sha256Hasher::hash(
                encoded_manifest
            );

        return (
            calculated_id ==
                entry.manifest_id
            && manifest.original_filename() ==
                entry.original_filename
            && manifest.file_size() ==
                entry.file_size
            && manifest.chunk_size() ==
                entry.chunk_size
            && manifest.chunk_count() ==
                entry.chunk_count
        );
    }
    catch (...)
    {
        return false;
    }
}

}
