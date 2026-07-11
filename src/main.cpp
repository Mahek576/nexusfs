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

        const std::filesystem::path file_path{argv[1]};
        const nexusfs::storage::Chunker chunker{default_chunk_size};

        const auto chunks = chunker.split_file(file_path);

        std::size_t total_bytes = 0;

        std::cout << "File: " << file_path << '\n';
        std::cout << "Configured chunk size: "
                  << chunker.chunk_size()
                  << " bytes\n";

        for (const auto& chunk : chunks)
        {
            std::cout << "Chunk "
                      << chunk.index
                      << ": "
                      << chunk.data.size()
                      << " bytes\n";

            total_bytes += chunk.data.size();
        }

        std::cout << "Total chunks: " << chunks.size() << '\n';
        std::cout << "Total bytes read: " << total_bytes << '\n';

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "NexusFS error: " << error.what() << '\n';
        return 1;
    }
}