#ifndef NEXUSFS_STORAGE_FILE_RECONSTRUCTOR_HPP
#define NEXUSFS_STORAGE_FILE_RECONSTRUCTOR_HPP

#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/file_manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace nexusfs::storage
{

struct ReconstructionResult
{
    std::uint64_t bytes_written;
    std::size_t chunks_loaded;
};

class FileReconstructor
{
public:
    [[nodiscard]] static ReconstructionResult reconstruct(
        const FileManifest& manifest,
        const ChunkStore& chunk_store,
        const std::filesystem::path& output_path
    );
};

}

#endif
