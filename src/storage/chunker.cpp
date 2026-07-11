#include "nexusfs/storage/chunker.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

namespace nexusfs::storage
{

Chunker::Chunker(std::size_t chunk_size)
    : chunk_size_{chunk_size}
{
    if (chunk_size_ == 0)
    {
        throw std::invalid_argument("Chunk size must be greater than zero.");
    }
}

std::size_t Chunker::chunk_size() const noexcept
{
    return chunk_size_;
}

std::vector<FileChunk> Chunker::split_file(
    const std::filesystem::path& file_path
) const
{
    if (!std::filesystem::exists(file_path))
    {
        throw std::runtime_error(
            "File does not exist: " + file_path.string()
        );
    }

    if (!std::filesystem::is_regular_file(file_path))
    {
        throw std::runtime_error(
            "Path is not a regular file: " + file_path.string()
        );
    }

    std::ifstream input{file_path, std::ios::binary};

    if (!input.is_open())
    {
        throw std::runtime_error(
            "Unable to open file: " + file_path.string()
        );
    }

    std::vector<FileChunk> chunks;
    std::size_t chunk_index = 0;

    while (true)
    {
        std::vector<std::uint8_t> buffer(chunk_size_);

        input.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size())
        );

        const std::streamsize bytes_read = input.gcount();

        if (bytes_read == 0)
        {
            break;
        }

        buffer.resize(static_cast<std::size_t>(bytes_read));

        chunks.push_back(
            FileChunk{
                chunk_index,
                std::move(buffer)
            }
        );

        ++chunk_index;
    }

    if (input.bad())
    {
        throw std::runtime_error(
            "An error occurred while reading file: " + file_path.string()
        );
    }

    return chunks;
}

}