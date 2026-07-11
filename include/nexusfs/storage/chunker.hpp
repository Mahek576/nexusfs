#ifndef NEXUSFS_STORAGE_CHUNKER_HPP
#define NEXUSFS_STORAGE_CHUNKER_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nexusfs::storage
{

struct FileChunk
{
    std::size_t index;
    std::vector<std::uint8_t> data;
    std::string hash;
};

class Chunker
{
public:
    explicit Chunker(std::size_t chunk_size);

    [[nodiscard]] std::size_t chunk_size() const noexcept;

    [[nodiscard]] std::vector<FileChunk> split_file(
        const std::filesystem::path& file_path
    ) const;

private:
    std::size_t chunk_size_;
};

}

#endif