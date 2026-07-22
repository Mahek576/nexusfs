#ifndef NEXUSFS_STORAGE_DURABLE_FILE_HPP
#define NEXUSFS_STORAGE_DURABLE_FILE_HPP

#include <filesystem>

namespace nexusfs::storage
{

enum class DurablePublishResult
{
    published,
    destination_exists
};

/*
 * Flushes the contents and metadata of an existing regular file to
 * stable storage.
 *
 * The caller must close any output stream writing the file before
 * calling this function.
 */
void flush_file_to_disk(
    const std::filesystem::path& file_path
);

/*
 * Publishes a temporary regular file without replacing an existing
 * destination.
 *
 * Preconditions:
 *
 * - temporary_path and destination_path are different;
 * - both paths have the same parent directory;
 * - the temporary file has been completely written and closed.
 *
 * Guarantees:
 *
 * - temporary bytes are flushed before publication;
 * - publication never overwrites an existing destination;
 * - successful publication persists directory metadata where the
 *   operating system provides the required primitive;
 * - destination_exists leaves the temporary file untouched.
 */
[[nodiscard]] DurablePublishResult
publish_file_durably(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& destination_path
);

}

#endif