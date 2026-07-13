#ifndef NEXUSFS_STORAGE_FILE_MANIFEST_CODEC_HPP
#define NEXUSFS_STORAGE_FILE_MANIFEST_CODEC_HPP

#include "nexusfs/storage/file_manifest.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace nexusfs::storage
{

class FileManifestCodec
{
public:
    [[nodiscard]] static std::vector<std::uint8_t> encode(
        const FileManifest& manifest
    );

    [[nodiscard]] static FileManifest decode(
        std::span<const std::uint8_t> encoded_manifest
    );
};

}

#endif