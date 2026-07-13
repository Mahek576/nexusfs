#include "nexusfs/cli/command_line.hpp"
#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/chunker.hpp"
#include "nexusfs/storage/file_manifest.hpp"
#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/file_reconstructor.hpp"
#include "nexusfs/storage/manifest_store.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace
{

constexpr std::size_t default_chunk_size = 1024;

const std::filesystem::path storage_root{
    "nexusfs_data"
};

int execute_store(
    const nexusfs::cli::StoreCommand& command
)
{
    const nexusfs::storage::Chunker chunker{
        default_chunk_size
    };

    nexusfs::storage::ChunkStore chunk_store{
        storage_root
    };

    nexusfs::storage::ManifestStore manifest_store{
        storage_root
    };

    const auto chunks =
        chunker.split_file(command.source_path);

    const auto manifest =
        nexusfs::storage::FileManifest::create(
            command.source_path,
            chunker.chunk_size(),
            chunks
        );

    const auto encoded_manifest =
        nexusfs::storage::FileManifestCodec::encode(
            manifest
        );

    const std::string manifest_id =
        nexusfs::storage::Sha256Hasher::hash(
            encoded_manifest
        );

    std::size_t stored_chunks = 0;
    std::size_t reused_chunks = 0;
    std::uint64_t total_bytes = 0;

    std::cout << "Command: store\n";

    std::cout << "Source file: "
              << command.source_path
              << '\n';

    std::cout << "Storage root: "
              << storage_root
              << '\n';

    std::cout << "Configured chunk size: "
              << chunker.chunk_size()
              << " bytes\n";

    for (const auto& chunk : chunks)
    {
        const auto result =
            chunk_store.store(chunk);

        if (
            result ==
            nexusfs::storage::StoreResult::stored
        )
        {
            ++stored_chunks;
        }
        else
        {
            ++reused_chunks;
        }

        const auto loaded_chunk =
            chunk_store.load(chunk.hash);

        std::cout
            << "Chunk "
            << chunk.index
            << ": "
            << chunk.data.size()
            << " bytes"
            << " | SHA-256: "
            << chunk.hash
            << " | "
            << (
                result ==
                nexusfs::storage::StoreResult::stored
                    ? "stored"
                    : "reused"
            )
            << " | verified "
            << loaded_chunk.size()
            << " bytes\n";

        total_bytes +=
            static_cast<std::uint64_t>(
                chunk.data.size()
            );
    }

    const auto manifest_result =
        manifest_store.store(
            manifest_id,
            encoded_manifest
        );

    const auto loaded_manifest_bytes =
        manifest_store.load(manifest_id);

    if (loaded_manifest_bytes != encoded_manifest)
    {
        throw std::runtime_error(
            "Stored manifest bytes do not match "
            "the encoded manifest."
        );
    }

    const auto decoded_manifest =
        nexusfs::storage::FileManifestCodec::decode(
            loaded_manifest_bytes
        );

    const auto reencoded_manifest =
        nexusfs::storage::FileManifestCodec::encode(
            decoded_manifest
        );

    if (reencoded_manifest != loaded_manifest_bytes)
    {
        throw std::runtime_error(
            "Stored manifest is not canonically encoded."
        );
    }

    std::cout << "Original filename: "
              << manifest.original_filename()
              << '\n';

    std::cout << "Original file size: "
              << manifest.file_size()
              << " bytes\n";

    std::cout << "Total chunks: "
              << manifest.chunk_count()
              << '\n';

    std::cout << "New chunks stored: "
              << stored_chunks
              << '\n';

    std::cout << "Existing chunks reused: "
              << reused_chunks
              << '\n';

    std::cout << "Total bytes processed: "
              << total_bytes
              << '\n';

    std::cout << "Encoded manifest size: "
              << encoded_manifest.size()
              << " bytes\n";

    std::cout << "Manifest storage result: "
              << (
                  manifest_result ==
                  nexusfs::storage::ManifestStoreResult::stored
                      ? "stored"
                      : "reused"
              )
              << '\n';

    std::cout << "Manifest ID: "
              << manifest_id
              << '\n';

    std::cout
        << "File stored successfully.\n";

    return 0;
}

int execute_restore(
    const nexusfs::cli::RestoreCommand& command
)
{
    nexusfs::storage::ChunkStore chunk_store{
        storage_root
    };

    nexusfs::storage::ManifestStore manifest_store{
        storage_root
    };

    const auto encoded_manifest =
        manifest_store.load(
            command.manifest_id
        );

    const auto manifest =
        nexusfs::storage::FileManifestCodec::decode(
            encoded_manifest
        );

    const auto canonical_manifest =
        nexusfs::storage::FileManifestCodec::encode(
            manifest
        );

    if (canonical_manifest != encoded_manifest)
    {
        throw std::runtime_error(
            "Stored manifest is not canonically encoded."
        );
    }

    const auto reconstruction_result =
        nexusfs::storage::FileReconstructor::reconstruct(
            manifest,
            chunk_store,
            command.output_path
        );

    std::cout << "Command: restore\n";

    std::cout << "Manifest ID: "
              << command.manifest_id
              << '\n';

    std::cout << "Original filename: "
              << manifest.original_filename()
              << '\n';

    std::cout << "Manifest file size: "
              << manifest.file_size()
              << " bytes\n";

    std::cout << "Manifest chunk references: "
              << manifest.chunk_count()
              << '\n';

    std::cout << "Output path: "
              << command.output_path
              << '\n';

    std::cout << "Reconstructed bytes written: "
              << reconstruction_result.bytes_written
              << '\n';

    std::cout << "Reconstructed chunks loaded: "
              << reconstruction_result.chunks_loaded
              << '\n';

    std::cout
        << "File restored successfully.\n";

    return 0;
}

int execute_inspect(
    const nexusfs::cli::InspectCommand& command
)
{
    nexusfs::storage::ChunkStore chunk_store{
        storage_root
    };

    nexusfs::storage::ManifestStore manifest_store{
        storage_root
    };

    const auto encoded_manifest =
        manifest_store.load(
            command.manifest_id
        );

    const auto manifest =
        nexusfs::storage::FileManifestCodec::decode(
            encoded_manifest
        );

    const auto canonical_manifest =
        nexusfs::storage::FileManifestCodec::encode(
            manifest
        );

    if (canonical_manifest != encoded_manifest)
    {
        throw std::runtime_error(
            "Stored manifest is not canonically encoded."
        );
    }

    std::size_t available_chunks = 0;
    std::size_t missing_chunks = 0;

    std::cout << "Command: inspect\n";

    std::cout << "Manifest ID: "
              << command.manifest_id
              << '\n';

    std::cout << "Storage root: "
              << storage_root
              << '\n';

    std::cout << "Manifest format version: "
              << manifest.format_version()
              << '\n';

    std::cout << "Original filename: "
              << manifest.original_filename()
              << '\n';

    std::cout << "Original file size: "
              << manifest.file_size()
              << " bytes\n";

    std::cout << "Configured chunk size: "
              << manifest.chunk_size()
              << " bytes\n";

    std::cout << "Encoded manifest size: "
              << encoded_manifest.size()
              << " bytes\n";

    std::cout << "Chunk references: "
              << manifest.chunk_count()
              << '\n';

    for (
        std::size_t index = 0;
        index < manifest.chunk_hashes().size();
        ++index
    )
    {
        const std::string& chunk_hash =
            manifest.chunk_hashes()[index];

        const bool is_available =
            chunk_store.contains(chunk_hash);

        if (is_available)
        {
            ++available_chunks;
        }
        else
        {
            ++missing_chunks;
        }

        std::cout << "Chunk reference "
                  << index
                  << ": "
                  << chunk_hash
                  << " | "
                  << (
                      is_available
                          ? "present"
                          : "missing"
                  )
                  << '\n';
    }

    std::cout << "Available chunks: "
              << available_chunks
              << '\n';

    std::cout << "Missing chunks: "
              << missing_chunks
              << '\n';

    std::cout << "Storage completeness: "
              << (
                  missing_chunks == 0
                      ? "complete"
                      : "incomplete"
              )
              << '\n';

    std::cout
        << "Manifest inspected successfully.\n";

    return 0;
}

}

int main(int argc, char* argv[])
{
    try
    {
        const nexusfs::cli::Command command =
            nexusfs::cli::CommandLineParser::parse(
                argc,
                argv
            );

        if (
            const auto* store_command =
                std::get_if<nexusfs::cli::StoreCommand>(
                    &command
                )
        )
        {
            return execute_store(
                *store_command
            );
        }

        if (
            const auto* restore_command =
                std::get_if<nexusfs::cli::RestoreCommand>(
                    &command
                )
        )
        {
            return execute_restore(
                *restore_command
            );
        }

        return execute_inspect(
            std::get<nexusfs::cli::InspectCommand>(
                command
            )
        );
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "NexusFS error: "
            << error.what()
            << '\n';

        return 1;
    }
}