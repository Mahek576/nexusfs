#ifndef NEXUSFS_STORAGE_FILE_VERIFIER_HPP
#define NEXUSFS_STORAGE_FILE_VERIFIER_HPP

#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/file_manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nexusfs::storage
{

struct VerifiedChunk
{
    std::size_t index;
    std::string hash;
    std::uint64_t bytes_verified;
};

struct FileVerificationResult
{
    std::uint64_t total_bytes_verified;
    std::vector<VerifiedChunk> verified_chunks;
};

class FileVerifier
{
public:
    [[nodiscard]] static FileVerificationResult verify(
        const FileManifest& manifest,
        const ChunkStore& chunk_store
    );
};

}

#endif