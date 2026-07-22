#include "nexusfs/storage/manifest_store.hpp"

#include "nexusfs/storage/durable_file.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace nexusfs::storage
{

namespace
{

constexpr std::size_t publication_read_attempts =
    250;

constexpr std::chrono::milliseconds
    publication_retry_delay{
        2
    };

enum class StoredPathState
{
    missing,
    regular_file,
    other
};

bool is_missing_path_error(
    const std::error_code& error
) noexcept
{
    return (
        error ==
            std::errc::no_such_file_or_directory
        || error ==
            std::errc::not_a_directory
    );
}

StoredPathState inspect_stored_path(
    const std::filesystem::path& path,
    const char* operation
)
{
    std::error_code status_error;

    const std::filesystem::file_status status =
        std::filesystem::status(
            path,
            status_error
        );

    if (status_error)
    {
        /*
         * Windows may report ERROR_PATH_NOT_FOUND when the
         * manifest shard directory has not been created yet.
         * That is a normal pre-publication state.
         */
        if (
            is_missing_path_error(
                status_error
            )
        )
        {
            return StoredPathState::missing;
        }

        throw std::runtime_error(
            std::string{
                "Failed to "
            }
            + operation
            + ": "
            + status_error.message()
        );
    }

    if (
        !std::filesystem::exists(
            status
        )
    )
    {
        return StoredPathState::missing;
    }

    if (
        std::filesystem::is_regular_file(
            status
        )
    )
    {
        return StoredPathState::regular_file;
    }

    return StoredPathState::other;
}

bool is_lowercase_hexadecimal(
    const std::string& value,
    std::size_t expected_length
)
{
    if (
        value.size() !=
        expected_length
    )
    {
        return false;
    }

    return std::all_of(
        value.begin(),
        value.end(),
        [](char character)
        {
            const bool is_decimal_digit =
                character >= '0'
                && character <= '9';

            const bool is_lowercase_hex_digit =
                character >= 'a'
                && character <= 'f';

            return (
                is_decimal_digit
                || is_lowercase_hex_digit
            );
        }
    );
}

std::size_t hexadecimal_nibble(
    char character
)
{
    if (
        character >= '0'
        && character <= '9'
    )
    {
        return static_cast<std::size_t>(
            character - '0'
        );
    }

    return (
        static_cast<std::size_t>(
            character - 'a'
        )
        + 10U
    );
}

std::mutex& publication_mutex_for(
    const std::string& manifest_id
)
{
    /*
     * Manifest IDs are validated before this function is called.
     *
     * The first byte of the SHA-256 identifier selects one of
     * 256 publication locks. Writers for the same manifest always
     * use the same lock, while unrelated shards remain concurrent.
     */
    static std::array<std::mutex, 256>
        publication_mutexes;

    const std::size_t high_nibble =
        hexadecimal_nibble(
            manifest_id[0]
        );

    const std::size_t low_nibble =
        hexadecimal_nibble(
            manifest_id[1]
        );

    const std::size_t mutex_index =
        high_nibble * 16U
        + low_nibble;

    return publication_mutexes[
        mutex_index
    ];
}

std::filesystem::path make_temporary_path(
    const std::filesystem::path& final_path
)
{
    static std::atomic<std::uint64_t> sequence{
        0
    };

    const auto timestamp =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    const std::uint64_t current_sequence =
        sequence.fetch_add(
            1,
            std::memory_order_relaxed
        );

    const std::size_t thread_token =
        std::hash<std::thread::id>{}(
            std::this_thread::get_id()
        );

    std::filesystem::path temporary_path =
        final_path;

    temporary_path +=
        ".tmp."
        + std::to_string(timestamp)
        + "."
        + std::to_string(thread_token)
        + "."
        + std::to_string(current_sequence);

    return temporary_path;
}

void remove_temporary_file(
    const std::filesystem::path& temporary_path
) noexcept
{
    std::error_code cleanup_error;

    std::filesystem::remove(
        temporary_path,
        cleanup_error
    );
}

std::vector<std::uint8_t>
read_and_verify_manifest_once(
    const std::filesystem::path& path,
    const std::string& expected_manifest_id
)
{
    std::error_code size_error;

    const std::uintmax_t file_size =
        std::filesystem::file_size(
            path,
            size_error
        );

    if (size_error)
    {
        throw std::runtime_error(
            "Failed to determine manifest size: "
            + size_error.message()
        );
    }

    if (file_size == 0)
    {
        throw std::runtime_error(
            "Stored manifest is empty: "
            + expected_manifest_id
        );
    }

    if (
        file_size >
        static_cast<std::uintmax_t>(
            std::numeric_limits<
                std::size_t
            >::max()
        )
    )
    {
        throw std::runtime_error(
            "Manifest is too large to load into memory."
        );
    }

    if (
        file_size >
        static_cast<std::uintmax_t>(
            std::numeric_limits<
                std::streamsize
            >::max()
        )
    )
    {
        throw std::runtime_error(
            "Manifest is too large "
            "for stream-based loading."
        );
    }

    std::ifstream input{
        path,
        std::ios::binary
    };

    if (!input.is_open())
    {
        throw std::runtime_error(
            "Failed to open manifest: "
            + expected_manifest_id
        );
    }

    std::vector<std::uint8_t> encoded_manifest(
        static_cast<std::size_t>(
            file_size
        )
    );

    input.read(
        reinterpret_cast<char*>(
            encoded_manifest.data()
        ),
        static_cast<std::streamsize>(
            encoded_manifest.size()
        )
    );

    if (!input)
    {
        throw std::runtime_error(
            "Failed while reading manifest: "
            + expected_manifest_id
        );
    }

    const std::string calculated_id =
        Sha256Hasher::hash(
            encoded_manifest
        );

    if (
        calculated_id !=
        expected_manifest_id
    )
    {
        throw std::runtime_error(
            "Manifest integrity verification failed: "
            + expected_manifest_id
        );
    }

    return encoded_manifest;
}

std::optional<std::vector<std::uint8_t>>
wait_for_published_manifest(
    const std::filesystem::path& final_path,
    const std::string& expected_manifest_id
)
{
    std::exception_ptr last_read_exception;

    for (
        std::size_t attempt = 0;
        attempt < publication_read_attempts;
        ++attempt
    )
    {
        const StoredPathState path_state =
            inspect_stored_path(
                final_path,
                "inspect a published manifest path"
            );

        if (
            path_state ==
            StoredPathState::regular_file
        )
        {
            try
            {
                return read_and_verify_manifest_once(
                    final_path,
                    expected_manifest_id
                );
            }
            catch (...)
            {
                /*
                 * On Windows, a renamed file can briefly appear
                 * in the directory before another thread can open
                 * it. Preserve the latest error and retry.
                 */
                last_read_exception =
                    std::current_exception();
            }
        }
        else if (
            path_state ==
            StoredPathState::other
        )
        {
            throw std::runtime_error(
                "Published manifest path is not "
                "a regular file: "
                + final_path.string()
            );
        }

        if (
            attempt + 1
            < publication_read_attempts
        )
        {
            std::this_thread::sleep_for(
                publication_retry_delay
            );
        }
    }

    if (last_read_exception)
    {
        std::rethrow_exception(
            last_read_exception
        );
    }

    return std::nullopt;
}

}

ManifestStore::ManifestStore(
    std::filesystem::path root_directory
)
    : root_directory_{
          std::move(root_directory)
      },
      manifests_directory_{
          root_directory_
          / "manifests"
      }
{
    if (root_directory_.empty())
    {
        throw std::invalid_argument(
            "Manifest store root directory cannot be empty."
        );
    }

    std::error_code directory_error;

    std::filesystem::create_directories(
        manifests_directory_,
        directory_error
    );

    if (directory_error)
    {
        throw std::runtime_error(
            "Failed to create manifest storage directory: "
            + directory_error.message()
        );
    }
}

ManifestStoreResult ManifestStore::store(
    const std::string& manifest_id,
    const std::vector<std::uint8_t>& encoded_manifest
)
{
    validate_manifest_id(
        manifest_id
    );

    if (encoded_manifest.empty())
    {
        throw std::invalid_argument(
            "Encoded manifest cannot be empty."
        );
    }

    if (
        encoded_manifest.size() >
        static_cast<std::size_t>(
            std::numeric_limits<
                std::streamsize
            >::max()
        )
    )
    {
        throw std::runtime_error(
            "Encoded manifest is too large "
            "for stream-based storage."
        );
    }

    const std::string calculated_id =
        Sha256Hasher::hash(
            encoded_manifest
        );

    if (
        calculated_id !=
        manifest_id
    )
    {
        throw std::runtime_error(
            "Encoded manifest does not match "
            "its manifest ID."
        );
    }

    /*
     * Serializes publication to the same content-addressed
     * manifest shard inside this process.
     */
    const std::unique_lock publication_lock{
        publication_mutex_for(
            manifest_id
        )
    };

    const std::filesystem::path final_path =
        manifest_path(
            manifest_id
        );

    const StoredPathState initial_path_state =
        inspect_stored_path(
            final_path,
            "inspect an existing manifest path"
        );

    if (
        initial_path_state ==
        StoredPathState::regular_file
    )
    {
        const auto existing_manifest =
            wait_for_published_manifest(
                final_path,
                manifest_id
            );

        if (!existing_manifest)
        {
            throw std::runtime_error(
                "Existing manifest disappeared before "
                "it could be verified: "
                + manifest_id
            );
        }

        if (
            *existing_manifest !=
            encoded_manifest
        )
        {
            throw std::runtime_error(
                "Existing manifest bytes do not match "
                "the supplied encoded manifest."
            );
        }

        return
            ManifestStoreResult::already_exists;
    }

    if (
        initial_path_state ==
        StoredPathState::other
    )
    {
        throw std::runtime_error(
            "Manifest destination exists but is not "
            "a regular file: "
            + final_path.string()
        );
    }

    const std::filesystem::path parent_directory =
        final_path.parent_path();

    std::error_code directory_error;

    std::filesystem::create_directories(
        parent_directory,
        directory_error
    );

    if (directory_error)
    {
        throw std::runtime_error(
            "Failed to create manifest shard directory: "
            + directory_error.message()
        );
    }

    const std::filesystem::path temporary_path =
        make_temporary_path(
            final_path
        );

    try
    {
        {
            std::ofstream output{
                temporary_path,
                std::ios::binary
                    | std::ios::trunc
            };

            if (!output.is_open())
            {
                throw std::runtime_error(
                    "Failed to create temporary manifest file: "
                    + temporary_path.string()
                );
            }

            output.write(
                reinterpret_cast<const char*>(
                    encoded_manifest.data()
                ),
                static_cast<std::streamsize>(
                    encoded_manifest.size()
                )
            );

            output.flush();

            if (!output)
            {
                throw std::runtime_error(
                    "Failed while writing manifest data."
                );
            }

            output.close();

            if (!output)
            {
                throw std::runtime_error(
                    "Failed while closing temporary "
                    "manifest file."
                );
            }
        }

        const StoredPathState temporary_path_state =
            inspect_stored_path(
                temporary_path,
                "inspect a temporary manifest path"
            );

        if (
            temporary_path_state !=
            StoredPathState::regular_file
        )
        {
            throw std::runtime_error(
                "Temporary manifest file was not "
                "created correctly."
            );
        }

        const std::vector<std::uint8_t>
            temporary_manifest =
                read_and_verify_manifest_once(
                    temporary_path,
                    manifest_id
                );

        if (
            temporary_manifest !=
            encoded_manifest
        )
        {
            throw std::runtime_error(
                "Temporary manifest bytes do not match "
                "the supplied encoded manifest."
            );
        }

        const DurablePublishResult publication_result =
            publish_file_durably(
                temporary_path,
                final_path
            );

        if (
            publication_result ==
            DurablePublishResult::destination_exists
        )
        {
            /*
             * A different process may have durably published the
             * same content-addressed manifest first.
             */
            const auto concurrently_published_manifest =
                wait_for_published_manifest(
                    final_path,
                    manifest_id
                );

            if (!concurrently_published_manifest)
            {
                throw std::runtime_error(
                    "Manifest destination exists but could not "
                    "be verified after durable publication: "
                    + manifest_id
                );
            }

            if (
                *concurrently_published_manifest !=
                encoded_manifest
            )
            {
                throw std::runtime_error(
                    "Concurrently stored manifest bytes "
                    "do not match the supplied manifest."
                );
            }

            remove_temporary_file(
                temporary_path
            );

            return
                ManifestStoreResult::already_exists;
        }
    }
    catch (...)
    {
        remove_temporary_file(
            temporary_path
        );

        throw;
    }

    const auto published_manifest =
        wait_for_published_manifest(
            final_path,
            manifest_id
        );

    if (!published_manifest)
    {
        throw std::runtime_error(
            "Published manifest is not visible "
            "after finalization: "
            + manifest_id
        );
    }

    if (
        *published_manifest !=
        encoded_manifest
    )
    {
        throw std::runtime_error(
            "Published manifest bytes do not match "
            "the supplied encoded manifest."
        );
    }

    return ManifestStoreResult::stored;
}

bool ManifestStore::contains(
    const std::string& manifest_id
) const
{
    validate_manifest_id(
        manifest_id
    );

    const StoredPathState path_state =
        inspect_stored_path(
            manifest_path(
                manifest_id
            ),
            "inspect a manifest path"
        );

    return (
        path_state ==
        StoredPathState::regular_file
    );
}

std::vector<std::uint8_t>
ManifestStore::load(
    const std::string& manifest_id
) const
{
    validate_manifest_id(
        manifest_id
    );

    const std::filesystem::path path =
        manifest_path(
            manifest_id
        );

    const StoredPathState path_state =
        inspect_stored_path(
            path,
            "inspect a manifest path"
        );

    if (
        path_state ==
        StoredPathState::missing
    )
    {
        throw std::runtime_error(
            "Manifest does not exist: "
            + manifest_id
        );
    }

    if (
        path_state ==
        StoredPathState::other
    )
    {
        throw std::runtime_error(
            "Manifest path is not a regular file: "
            + manifest_id
        );
    }

    const auto encoded_manifest =
        wait_for_published_manifest(
            path,
            manifest_id
        );

    if (!encoded_manifest)
    {
        throw std::runtime_error(
            "Manifest disappeared before "
            "it could be loaded: "
            + manifest_id
        );
    }

    return *encoded_manifest;
}

std::vector<std::string>
ManifestStore::list_manifest_ids() const
{
    std::vector<std::string> manifest_ids;

    std::error_code shard_error;

    std::filesystem::directory_iterator
        shard_iterator{
            manifests_directory_,
            shard_error
        };

    if (shard_error)
    {
        throw std::runtime_error(
            "Failed to enumerate manifest shards: "
            + shard_error.message()
        );
    }

    const std::filesystem::directory_iterator end;

    while (
        shard_iterator != end
    )
    {
        const std::filesystem::directory_entry&
            shard_entry =
                *shard_iterator;

        std::error_code type_error;

        const bool is_directory =
            shard_entry.is_directory(
                type_error
            );

        if (type_error)
        {
            throw std::runtime_error(
                "Failed to inspect manifest shard entry: "
                + type_error.message()
            );
        }

        if (is_directory)
        {
            const std::string shard_name =
                shard_entry.path()
                    .filename()
                    .string();

            if (
                is_lowercase_hexadecimal(
                    shard_name,
                    2
                )
            )
            {
                std::error_code manifest_error;

                std::filesystem::directory_iterator
                    manifest_iterator{
                        shard_entry.path(),
                        manifest_error
                    };

                if (manifest_error)
                {
                    throw std::runtime_error(
                        "Failed to enumerate manifest shard: "
                        + manifest_error.message()
                    );
                }

                while (
                    manifest_iterator != end
                )
                {
                    const std::filesystem::
                        directory_entry&
                            manifest_entry =
                                *manifest_iterator;

                    std::error_code file_type_error;

                    const bool is_regular_file =
                        manifest_entry.is_regular_file(
                            file_type_error
                        );

                    if (file_type_error)
                    {
                        throw std::runtime_error(
                            "Failed to inspect manifest entry: "
                            + file_type_error.message()
                        );
                    }

                    if (is_regular_file)
                    {
                        const std::string manifest_name =
                            manifest_entry.path()
                                .filename()
                                .string();

                        /*
                         * Temporary files contain suffixes and
                         * therefore cannot satisfy this exact
                         * 62-character hexadecimal validation.
                         */
                        if (
                            is_lowercase_hexadecimal(
                                manifest_name,
                                62
                            )
                        )
                        {
                            manifest_ids.push_back(
                                shard_name
                                + manifest_name
                            );
                        }
                    }

                    manifest_iterator.increment(
                        manifest_error
                    );

                    if (manifest_error)
                    {
                        throw std::runtime_error(
                            "Failed while enumerating "
                            "manifest shard: "
                            + manifest_error.message()
                        );
                    }
                }
            }
        }

        shard_iterator.increment(
            shard_error
        );

        if (shard_error)
        {
            throw std::runtime_error(
                "Failed while enumerating "
                "manifest shards: "
                + shard_error.message()
            );
        }
    }

    std::sort(
        manifest_ids.begin(),
        manifest_ids.end()
    );

    manifest_ids.erase(
        std::unique(
            manifest_ids.begin(),
            manifest_ids.end()
        ),
        manifest_ids.end()
    );

    return manifest_ids;
}

const std::filesystem::path&
ManifestStore::root_directory() const noexcept
{
    return root_directory_;
}

std::filesystem::path
ManifestStore::manifest_path(
    const std::string& manifest_id
) const
{
    validate_manifest_id(
        manifest_id
    );

    return manifests_directory_
        / manifest_id.substr(
            0,
            2
        )
        / manifest_id.substr(
            2
        );
}

void ManifestStore::validate_manifest_id(
    const std::string& manifest_id
)
{
    constexpr std::size_t sha256_hex_length =
        64;

    if (
        manifest_id.size() !=
        sha256_hex_length
    )
    {
        throw std::invalid_argument(
            "Manifest ID must contain "
            "64 hexadecimal characters."
        );
    }

    const bool is_valid =
        std::all_of(
            manifest_id.begin(),
            manifest_id.end(),
            [](char character)
            {
                const bool is_decimal_digit =
                    character >= '0'
                    && character <= '9';

                const bool is_lowercase_hex_digit =
                    character >= 'a'
                    && character <= 'f';

                return (
                    is_decimal_digit
                    || is_lowercase_hex_digit
                );
            }
        );

    if (!is_valid)
    {
        throw std::invalid_argument(
            "Manifest ID must contain only "
            "lowercase hexadecimal characters."
        );
    }
}

}