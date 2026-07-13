#ifndef NEXUSFS_STORAGE_FILE_MANIFEST_HPP
#define NEXUSFS_STORAGE_FILE_MANIFEST_HPP

#include "nexusfs/storage/chunker.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nexusfs::storage
{

class FileManifest
{
public:
    static constexpr std::uint32_t current_format_version = 1;

    [[nodiscard]] static FileManifest create(
        const std::filesystem::path& file_path,
        std::size_t chunk_size,
        const std::vector<FileChunk>& chunks
    );

    [[nodiscard]] static FileManifest restore(
        std::string original_filename,
        std::uint64_t file_size,
        std::size_t chunk_size,
        std::vector<std::string> chunk_hashes
    );

    [[nodiscard]] std::uint32_t
    format_version() const noexcept;

    [[nodiscard]] const std::string&
    original_filename() const noexcept;

    [[nodiscard]] std::uint64_t
    file_size() const noexcept;

    [[nodiscard]] std::size_t
    chunk_size() const noexcept;

    [[nodiscard]] std::size_t
    chunk_count() const noexcept;

    [[nodiscard]] const std::vector<std::string>&
    chunk_hashes() const noexcept;

private:
    FileManifest(
        std::string original_filename,
        std::uint64_t file_size,
        std::size_t chunk_size,
        std::vector<std::string> chunk_hashes
    );

    std::string original_filename_;
    std::uint64_t file_size_;
    std::size_t chunk_size_;
    std::vector<std::string> chunk_hashes_;
};

}

#endif