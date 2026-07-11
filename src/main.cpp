#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/chunker.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>

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

        const auto chunks =
            chunker.split_file(file_path);

        std::size_t total_bytes = 0;
        std::size_t stored_chunks = 0;
        std::size_t reused_chunks = 0;

        std::cout << "File: "
                  << file_path
                  << '\n';

        std::cout << "Configured chunk size: "
                  << chunker.chunk_size()
                  << " bytes\n";

        std::cout << "Chunk store: "
                  << chunk_store.root_directory()
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