#include "nexusfs/storage/file_verifier.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace nexusfs::storage
{

namespace
{

std::uint64_t convert_size_to_uint64(
    std::size_t value,
    const char* error_message
)
{
    if constexpr (
        sizeof(std::size_t) >
        sizeof(std::uint64_t)
    )
    {
        if (
            value >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max()
            )
        )
        {
            throw std::overflow_error(error_message);
        }
    }

    return static_cast<std::uint64_t>(value);
}

}

FileVerificationResult FileVerifier::verify(
    const FileManifest& manifest,
    const ChunkStore& chunk_store
)
{
    if (manifest.chunk_size() == 0)
    {
        throw std::runtime_error(
            "Cannot verify a file with a zero chunk size."
        );
    }

    const std::uint64_t configured_chunk_size =
        convert_size_to_uint64(
            manifest.chunk_size(),
            "Manifest chunk size exceeds the verification limit."
        );

    std::uint64_t total_bytes_verified = 0;

    std::vector<VerifiedChunk> verified_chunks;
    verified_chunks.reserve(
        manifest.chunk_count()
    );

    for (
        std::size_t index = 0;
        index < manifest.chunk_hashes().size();
        ++index
    )
    {
        if (
            total_bytes_verified >
            manifest.file_size()
        )
        {
            throw std::runtime_error(
                "Verified byte count exceeds the manifest file size."
            );
        }

        const std::string& chunk_hash =
            manifest.chunk_hashes()[index];

        const auto chunk_data =
            chunk_store.load(chunk_hash);

        const std::uint64_t remaining_bytes =
            manifest.file_size()
            - total_bytes_verified;

        const std::uint64_t expected_chunk_size =
            std::min(
                configured_chunk_size,
                remaining_bytes
            );

        const std::uint64_t actual_chunk_size =
            convert_size_to_uint64(
                chunk_data.size(),
                "Loaded chunk size exceeds the verification limit."
            );

        if (
            actual_chunk_size !=
            expected_chunk_size
        )
        {
            throw std::runtime_error(
                "Stored chunk size is inconsistent with the manifest "
                "at chunk index "
                + std::to_string(index)
                + "."
            );
        }

        verified_chunks.push_back(
            VerifiedChunk{
                index,
                chunk_hash,
                actual_chunk_size
            }
        );

        total_bytes_verified +=
            actual_chunk_size;
    }

    if (
        verified_chunks.size() !=
        manifest.chunk_count()
    )
    {
        throw std::runtime_error(
            "Verified chunk count does not match the manifest."
        );
    }

    if (
        total_bytes_verified !=
        manifest.file_size()
    )
    {
        throw std::runtime_error(
            "Verified byte count does not match the manifest file size."
        );
    }

    return FileVerificationResult{
        total_bytes_verified,
        std::move(verified_chunks)
    };
}

}