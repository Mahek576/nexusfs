#ifndef NEXUSFS_STORAGE_STORAGE_RECOVERY_HPP
#define NEXUSFS_STORAGE_STORAGE_RECOVERY_HPP

#include <cstdint>
#include <filesystem>

namespace nexusfs::storage
{

struct StorageRecoveryReport
{
    std::uint64_t entries_scanned{0};
    std::uint64_t temporary_entries_found{0};
    std::uint64_t temporary_files_removed{0};
    std::uint64_t non_regular_entries_preserved{0};
};

/*
 * Scans the NexusFS-managed chunk and manifest directories for
 * abandoned publication files.
 *
 * A valid NexusFS temporary filename ends with:
 *
 *     .tmp.<timestamp>.<thread-token>.<sequence>
 *
 * Regular temporary files are removed. Directories, symbolic links
 * and other non-regular entries are preserved and reported.
 *
 * The function is idempotent. Running it repeatedly cannot remove
 * published chunk or manifest paths.
 */
[[nodiscard]] StorageRecoveryReport
recover_storage_root(
    const std::filesystem::path& storage_root
);

}

#endif
