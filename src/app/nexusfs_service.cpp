#include "nexusfs/app/nexusfs_service.hpp"

#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/chunker.hpp"
#include "nexusfs/storage/file_manifest.hpp"
#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/file_reconstructor.hpp"
#include "nexusfs/storage/file_verifier.hpp"
#include "nexusfs/storage/manifest_store.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nexusfs::app
{

namespace
{

struct LoadedManifest
{
    std::vector<std::uint8_t> encoded_manifest;
    storage::FileManifest manifest;
};

LoadedManifest load_canonical_manifest(
    const storage::ManifestStore& manifest_store,
    const std::string& manifest_id
)
{
    auto encoded_manifest =
        manifest_store.load(manifest_id);

    auto manifest =
        storage::FileManifestCodec::decode(
            encoded_manifest
        );

    const auto canonical_manifest =
        storage::FileManifestCodec::encode(
            manifest
        );

    if (canonical_manifest != encoded_manifest)
    {
        throw std::runtime_error(
            "Stored manifest is not canonically encoded: "
            + manifest_id
        );
    }

    return LoadedManifest{
        std::move(encoded_manifest),
        std::move(manifest)
    };
}

}

NexusFsService::NexusFsService(
    std::filesystem::path storage_root,
    std::size_t default_chunk_size
)
    : storage_root_{std::move(storage_root)},
      default_chunk_size_{default_chunk_size}
{
    if (storage_root_.empty())
    {
        throw std::invalid_argument(
            "NexusFS storage root cannot be empty."
        );
    }

    if (default_chunk_size_ == 0)
    {
        throw std::invalid_argument(
            "NexusFS default chunk size must be greater than zero."
        );
    }
}

StoreFileResult NexusFsService::store_file(
    const std::filesystem::path& source_path
) const
{
    if (source_path.empty())
    {
        throw std::invalid_argument(
            "Source file path cannot be empty."
        );
    }

    const storage::Chunker chunker{
        default_chunk_size_
    };

    storage::ChunkStore chunk_store{
        storage_root_
    };

    storage::ManifestStore manifest_store{
        storage_root_
    };

    const auto chunks =
        chunker.split_file(source_path);

    const auto manifest =
        storage::FileManifest::create(
            source_path,
            chunker.chunk_size(),
            chunks
        );

    const auto encoded_manifest =
        storage::FileManifestCodec::encode(
            manifest
        );

    const std::string manifest_id =
        storage::Sha256Hasher::hash(
            encoded_manifest
        );

    std::size_t chunks_stored = 0;
    std::size_t chunks_reused = 0;

    for (const auto& chunk : chunks)
    {
        const storage::StoreResult store_result =
            chunk_store.store(chunk);

        if (
            store_result ==
            storage::StoreResult::stored
        )
        {
            ++chunks_stored;
        }
        else
        {
            ++chunks_reused;
        }

        const auto loaded_chunk =
            chunk_store.load(chunk.hash);

        if (loaded_chunk != chunk.data)
        {
            throw std::runtime_error(
                "Stored chunk data does not match "
                "the source chunk at index "
                + std::to_string(chunk.index)
                + "."
            );
        }
    }

    const storage::ManifestStoreResult
        manifest_store_result =
            manifest_store.store(
                manifest_id,
                encoded_manifest
            );

    const LoadedManifest loaded_manifest =
        load_canonical_manifest(
            manifest_store,
            manifest_id
        );

    if (
        loaded_manifest.encoded_manifest !=
        encoded_manifest
    )
    {
        throw std::runtime_error(
            "Stored manifest bytes do not match "
            "the encoded source manifest."
        );
    }

    return StoreFileResult{
        source_path,
        manifest_id,
        manifest.original_filename(),
        manifest.file_size(),
        manifest.chunk_count(),
        chunks_stored,
        chunks_reused,
        manifest.file_size(),
        encoded_manifest.size(),
        manifest_store_result ==
            storage::ManifestStoreResult::stored
    };
}

RestoreFileResult NexusFsService::restore_file(
    const std::string& manifest_id,
    const std::filesystem::path& output_path
) const
{
    if (manifest_id.empty())
    {
        throw std::invalid_argument(
            "Restore manifest ID cannot be empty."
        );
    }

    if (output_path.empty())
    {
        throw std::invalid_argument(
            "Restore output path cannot be empty."
        );
    }

    storage::ChunkStore chunk_store{
        storage_root_
    };

    storage::ManifestStore manifest_store{
        storage_root_
    };

    const LoadedManifest loaded_manifest =
        load_canonical_manifest(
            manifest_store,
            manifest_id
        );

    const storage::ReconstructionResult
        reconstruction_result =
            storage::FileReconstructor::reconstruct(
                loaded_manifest.manifest,
                chunk_store,
                output_path
            );

    return RestoreFileResult{
        manifest_id,
        loaded_manifest.manifest.original_filename(),
        loaded_manifest.manifest.file_size(),
        loaded_manifest.manifest.chunk_count(),
        output_path,
        reconstruction_result.bytes_written,
        reconstruction_result.chunks_loaded
    };
}

InspectFileResult NexusFsService::inspect_file(
    const std::string& manifest_id
) const
{
    if (manifest_id.empty())
    {
        throw std::invalid_argument(
            "Inspect manifest ID cannot be empty."
        );
    }

    storage::ChunkStore chunk_store{
        storage_root_
    };

    storage::ManifestStore manifest_store{
        storage_root_
    };

    const LoadedManifest loaded_manifest =
        load_canonical_manifest(
            manifest_store,
            manifest_id
        );

    std::vector<InspectedChunk> inspected_chunks;
    inspected_chunks.reserve(
        loaded_manifest.manifest.chunk_count()
    );

    std::size_t available_chunks = 0;
    std::size_t missing_chunks = 0;

    const auto& chunk_hashes =
        loaded_manifest.manifest.chunk_hashes();

    for (
        std::size_t index = 0;
        index < chunk_hashes.size();
        ++index
    )
    {
        const std::string& chunk_hash =
            chunk_hashes[index];

        const bool is_present =
            chunk_store.contains(chunk_hash);

        if (is_present)
        {
            ++available_chunks;
        }
        else
        {
            ++missing_chunks;
        }

        inspected_chunks.push_back(
            InspectedChunk{
                index,
                chunk_hash,
                is_present
            }
        );
    }

    return InspectFileResult{
        manifest_id,
        loaded_manifest.manifest.original_filename(),
        loaded_manifest.manifest.file_size(),
        loaded_manifest.manifest.chunk_size(),
        loaded_manifest.encoded_manifest.size(),
        std::move(inspected_chunks),
        available_chunks,
        missing_chunks
    };
}

VerifyFileResult NexusFsService::verify_file(
    const std::string& manifest_id
) const
{
    if (manifest_id.empty())
    {
        throw std::invalid_argument(
            "Verify manifest ID cannot be empty."
        );
    }

    storage::ChunkStore chunk_store{
        storage_root_
    };

    storage::ManifestStore manifest_store{
        storage_root_
    };

    const LoadedManifest loaded_manifest =
        load_canonical_manifest(
            manifest_store,
            manifest_id
        );

    const storage::FileVerificationResult
        verification_result =
            storage::FileVerifier::verify(
                loaded_manifest.manifest,
                chunk_store
            );

    std::vector<VerifiedChunkResult>
        verified_chunks;

    verified_chunks.reserve(
        verification_result.verified_chunks.size()
    );

    for (
        const storage::VerifiedChunk& verified_chunk :
        verification_result.verified_chunks
    )
    {
        verified_chunks.push_back(
            VerifiedChunkResult{
                verified_chunk.index,
                verified_chunk.hash,
                verified_chunk.bytes_verified
            }
        );
    }

    return VerifyFileResult{
        manifest_id,
        loaded_manifest.manifest.original_filename(),
        loaded_manifest.manifest.file_size(),
        loaded_manifest.manifest.chunk_count(),
        std::move(verified_chunks),
        verification_result.total_bytes_verified
    };
}

ListFilesResult NexusFsService::list_files() const
{
    storage::ChunkStore chunk_store{
        storage_root_
    };

    storage::ManifestStore manifest_store{
        storage_root_
    };

    const std::vector<std::string> manifest_ids =
        manifest_store.list_manifest_ids();

    std::vector<StoredFileSummary> files;
    files.reserve(manifest_ids.size());

    std::size_t complete_manifests = 0;
    std::size_t incomplete_manifests = 0;

    for (const std::string& manifest_id : manifest_ids)
    {
        const LoadedManifest loaded_manifest =
            load_canonical_manifest(
                manifest_store,
                manifest_id
            );

        std::size_t missing_chunks = 0;

        for (
            const std::string& chunk_hash :
            loaded_manifest.manifest.chunk_hashes()
        )
        {
            if (!chunk_store.contains(chunk_hash))
            {
                ++missing_chunks;
            }
        }

        if (missing_chunks == 0)
        {
            ++complete_manifests;
        }
        else
        {
            ++incomplete_manifests;
        }

        files.push_back(
            StoredFileSummary{
                manifest_id,
                loaded_manifest.manifest
                    .original_filename(),
                loaded_manifest.manifest.file_size(),
                loaded_manifest.manifest.chunk_size(),
                loaded_manifest.manifest.chunk_count(),
                missing_chunks
            }
        );
    }

    return ListFilesResult{
        std::move(files),
        complete_manifests,
        incomplete_manifests
    };
}

const std::filesystem::path&
NexusFsService::storage_root() const noexcept
{
    return storage_root_;
}

std::size_t
NexusFsService::default_chunk_size() const noexcept
{
    return default_chunk_size_;
}

}