#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/chunker.hpp"
#include "nexusfs/storage/file_manifest.hpp"
#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/file_reconstructor.hpp"
#include "nexusfs/storage/manifest_store.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr << "Usage: nexusfs <file-path>\n";
        return 1;
    }

    try
    {
        constexpr std::size_t default_chunk_size = 1024;

        const std::filesystem::path file_path{
            argv[1]
        };

        const nexusfs::storage::Chunker chunker{
            default_chunk_size
        };

        nexusfs::storage::ChunkStore chunk_store{
            "nexusfs_data"
        };

        nexusfs::storage::ManifestStore manifest_store{
            "nexusfs_data"
        };

        const auto chunks =
            chunker.split_file(file_path);

        const auto manifest =
            nexusfs::storage::FileManifest::create(
                file_path,
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

        std::size_t total_bytes = 0;
        std::size_t stored_chunks = 0;
        std::size_t reused_chunks = 0;

        std::cout << "File: "
                  << file_path
                  << '\n';

        std::cout << "Configured chunk size: "
                  << chunker.chunk_size()
                  << " bytes\n";

        std::cout << "Storage root: "
                  << chunk_store.root_directory()
                  << '\n';

        std::cout << "Manifest format version: "
                  << manifest.format_version()
                  << '\n';

        std::cout << "Manifest filename: "
                  << manifest.original_filename()
                  << '\n';

        std::cout << "Manifest file size: "
                  << manifest.file_size()
                  << " bytes\n";

        std::cout << "Manifest chunk size: "
                  << manifest.chunk_size()
                  << " bytes\n";

        std::cout << "Manifest chunk references: "
                  << manifest.chunk_count()
                  << '\n';

        std::cout << "Encoded manifest size: "
                  << encoded_manifest.size()
                  << " bytes\n";

        std::cout << "Manifest ID: "
                  << manifest_id
                  << '\n';

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

            const auto loaded_data =
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
                << loaded_data.size()
                << " bytes\n";

            total_bytes += chunk.data.size();
        }

        const auto manifest_store_result =
            manifest_store.store(
                manifest_id,
                encoded_manifest
            );

        const auto loaded_manifest_bytes =
            manifest_store.load(manifest_id);

        if (loaded_manifest_bytes != encoded_manifest)
        {
            throw std::runtime_error(
                "Loaded manifest bytes do not match the encoded manifest."
            );
        }

        const auto decoded_manifest =
            nexusfs::storage::FileManifestCodec::decode(
                loaded_manifest_bytes
            );

        if (
            decoded_manifest.format_version() !=
                manifest.format_version() ||
            decoded_manifest.original_filename() !=
                manifest.original_filename() ||
            decoded_manifest.file_size() !=
                manifest.file_size() ||
            decoded_manifest.chunk_size() !=
                manifest.chunk_size() ||
            decoded_manifest.chunk_hashes() !=
                manifest.chunk_hashes()
        )
        {
            throw std::runtime_error(
                "Decoded manifest metadata does not match the original manifest."
            );
        }

        const auto reencoded_manifest =
            nexusfs::storage::FileManifestCodec::encode(
                decoded_manifest
            );

        if (reencoded_manifest != loaded_manifest_bytes)
        {
            throw std::runtime_error(
                "Decoded manifest does not reproduce the canonical encoding."
            );
        }

        const std::filesystem::path reconstruction_path =
            std::filesystem::path{"reconstructed"} /
            decoded_manifest.original_filename();

        const auto reconstruction_result =
            nexusfs::storage::FileReconstructor::reconstruct(
                decoded_manifest,
                chunk_store,
                reconstruction_path
            );

        std::cout << "Total chunks: "
                  << chunks.size()
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

        std::cout << "Manifest storage result: "
                  << (
                      manifest_store_result ==
                      nexusfs::storage::ManifestStoreResult::stored
                          ? "stored"
                          : "reused"
                  )
                  << '\n';

        std::cout << "Manifest bytes verified: "
                  << loaded_manifest_bytes.size()
                  << " bytes\n";

        std::cout << "Decoded manifest filename: "
                  << decoded_manifest.original_filename()
                  << '\n';

        std::cout << "Decoded manifest file size: "
                  << decoded_manifest.file_size()
                  << " bytes\n";

        std::cout << "Decoded manifest chunk references: "
                  << decoded_manifest.chunk_count()
                  << '\n';

        std::cout << "Manifest round-trip verified: "
                  << reencoded_manifest.size()
                  << " bytes\n";

        std::cout << "Reconstructed output: "
                  << reconstruction_path
                  << '\n';

        std::cout << "Reconstructed bytes written: "
                  << reconstruction_result.bytes_written
                  << '\n';

        std::cout << "Reconstructed chunks loaded: "
                  << reconstruction_result.chunks_loaded
                  << '\n';

        std::cout
            << "File reconstructed from persistent chunks successfully.\n";

        return 0;
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