#ifndef NEXUSFS_STORAGE_CHUNK_STORE_HPP
#define NEXUSFS_STORAGE_CHUNK_STORE_HPP

#include "nexusfs/storage/chunker.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nexusfs::storage
{

enum class StoreResult
{
    stored,
    already_exists
};

class ChunkStore
{
public:
    explicit ChunkStore(std::filesystem::path root_directory);

    [[nodiscard]] StoreResult store(const FileChunk& chunk);

    [[nodiscard]] bool contains(const std::string& hash) const;

    [[nodiscard]] std::vector<std::uint8_t> load(
        const std::string& hash
    ) const;

    [[nodiscard]] const std::filesystem::path&
    root_directory() const noexcept;

private:
    [[nodiscard]] std::filesystem::path chunk_path(
        const std::string& hash
    ) const;

    static void validate_hash(const std::string& hash);

    std::filesystem::path root_directory_;
    std::filesystem::path chunks_directory_;
};

}

#endif