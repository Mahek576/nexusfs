#include "nexusfs/storage/chunk_store.hpp"

#include "nexusfs/storage/durable_file.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
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
         * Windows can report ERROR_PATH_NOT_FOUND when the
         * shard directory does not exist yet. That is a normal
         * missing-path result during first publication.
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

std::filesystem::path make_temporary_path(
    const std::filesystem::path& final_path
)
{
    /*
     * Timestamp, thread identity and an atomic sequence make
     * each temporary path unique inside this process.
     */
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
read_and_verify_chunk_once(
    const std::filesystem::path& path,
    const std::string& expected_hash
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
            "Failed to determine chunk size: "
            + size_error.message()
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
            "Chunk is too large to load into memory."
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
            "Chunk is too large for stream-based loading."
        );
    }

    std::ifstream input{
        path,
        std::ios::binary
    };

    if (!input.is_open())
    {
        throw std::runtime_error(
            "Failed to open chunk: "
            + expected_hash
        );
    }

    std::vector<std::uint8_t> data(
        static_cast<std::size_t>(
            file_size
        )
    );

    if (!data.empty())
    {
        input.read(
            reinterpret_cast<char*>(
                data.data()
            ),
            static_cast<std::streamsize>(
                data.size()
            )
        );

        if (!input)
        {
            throw std::runtime_error(
                "Failed while reading chunk: "
                + expected_hash
            );
        }
    }

    const std::string calculated_hash =
        Sha256Hasher::hash(
            data
        );

    if (
        calculated_hash !=
        expected_hash
    )
    {
        throw std::runtime_error(
            "Chunk integrity verification failed: "
            + expected_hash
        );
    }

    return data;
}

std::optional<std::vector<std::uint8_t>>
wait_for_published_chunk(
    const std::filesystem::path& final_path,
    const std::string& expected_hash
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
                "inspect a published chunk path"
            );

        if (
            path_state ==
            StoredPathState::regular_file
        )
        {
            try
            {
                return read_and_verify_chunk_once(
                    final_path,
                    expected_hash
                );
            }
            catch (...)
            {
                /*
                 * A renamed file can briefly be visible before
                 * another Windows thread can open it. Keep the
                 * most recent error and retry for a bounded time.
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
                "Published chunk path is not "
                "a regular file: "
                + final_path.string()
            );
        }

        if (
            attempt + 1 <
            publication_read_attempts
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

ChunkStore::ChunkStore(
    std::filesystem::path root_directory
)
    : root_directory_{
          std::move(root_directory)
      },
      chunks_directory_{
          root_directory_
          / "chunks"
      }
{
    if (root_directory_.empty())
    {
        throw std::invalid_argument(
            "Chunk store root directory cannot be empty."
        );
    }

    std::error_code directory_error;

    std::filesystem::create_directories(
        chunks_directory_,
        directory_error
    );

    if (directory_error)
    {
        throw std::runtime_error(
            "Failed to create chunk storage directory: "
            + directory_error.message()
        );
    }
}

StoreResult ChunkStore::store(
    const FileChunk& chunk
)
{
    validate_hash(
        chunk.hash
    );

    const std::string calculated_hash =
        Sha256Hasher::hash(
            chunk.data
        );

    if (
        calculated_hash !=
        chunk.hash
    )
    {
        throw std::runtime_error(
            "Chunk data does not match its SHA-256 hash."
        );
    }

    const std::filesystem::path final_path =
        chunk_path(
            chunk.hash
        );

    const StoredPathState initial_path_state =
        inspect_stored_path(
            final_path,
            "inspect an existing chunk path"
        );

    if (
        initial_path_state ==
        StoredPathState::regular_file
    )
    {
        const auto existing_data =
            wait_for_published_chunk(
                final_path,
                chunk.hash
            );

        if (!existing_data)
        {
            throw std::runtime_error(
                "Existing chunk disappeared before "
                "it could be verified: "
                + chunk.hash
            );
        }

        if (
            *existing_data !=
            chunk.data
        )
        {
            throw std::runtime_error(
                "Existing chunk bytes do not match "
                "the supplied chunk data."
            );
        }

        return StoreResult::already_exists;
    }

    if (
        initial_path_state ==
        StoredPathState::other
    )
    {
        throw std::runtime_error(
            "Chunk destination exists but is not "
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
            "Failed to create chunk directory: "
            + directory_error.message()
        );
    }

    if (
        chunk.data.size() >
        static_cast<std::size_t>(
            std::numeric_limits<
                std::streamsize
            >::max()
        )
    )
    {
        throw std::runtime_error(
            "Chunk is too large for stream-based storage."
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
                    "Failed to create temporary chunk file: "
                    + temporary_path.string()
                );
            }

            if (!chunk.data.empty())
            {
                output.write(
                    reinterpret_cast<const char*>(
                        chunk.data.data()
                    ),
                    static_cast<std::streamsize>(
                        chunk.data.size()
                    )
                );
            }

            output.flush();

            if (!output)
            {
                throw std::runtime_error(
                    "Failed while writing chunk data."
                );
            }

            output.close();

            if (!output)
            {
                throw std::runtime_error(
                    "Failed while closing temporary chunk file."
                );
            }
        }

        const StoredPathState temporary_path_state =
            inspect_stored_path(
                temporary_path,
                "inspect a temporary chunk path"
            );

        if (
            temporary_path_state !=
            StoredPathState::regular_file
        )
        {
            throw std::runtime_error(
                "Temporary chunk file was not "
                "created correctly."
            );
        }

        const std::vector<std::uint8_t>
            temporary_data =
                read_and_verify_chunk_once(
                    temporary_path,
                    chunk.hash
                );

        if (
            temporary_data !=
            chunk.data
        )
        {
            throw std::runtime_error(
                "Temporary chunk bytes do not match "
                "the supplied chunk data."
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
             * Another writer or process published the identical
             * content-addressed chunk first.
             */
            const auto concurrently_published_data =
                wait_for_published_chunk(
                    final_path,
                    chunk.hash
                );

            if (!concurrently_published_data)
            {
                throw std::runtime_error(
                    "Chunk destination exists but could not "
                    "be verified after durable publication: "
                    + chunk.hash
                );
            }

            if (
                *concurrently_published_data !=
                chunk.data
            )
            {
                throw std::runtime_error(
                    "Concurrently stored chunk bytes "
                    "do not match the supplied data."
                );
            }

            remove_temporary_file(
                temporary_path
            );

            return StoreResult::already_exists;
        }
    }
    catch (...)
    {
        remove_temporary_file(
            temporary_path
        );

        throw;
    }

    const auto published_data =
        wait_for_published_chunk(
            final_path,
            chunk.hash
        );

    if (!published_data)
    {
        throw std::runtime_error(
            "Published chunk is not visible "
            "after finalization: "
            + chunk.hash
        );
    }

    if (
        *published_data !=
        chunk.data
    )
    {
        throw std::runtime_error(
            "Published chunk bytes do not match "
            "the supplied chunk data."
        );
    }

    return StoreResult::stored;
}

bool ChunkStore::contains(
    const std::string& hash
) const
{
    validate_hash(
        hash
    );

    const StoredPathState path_state =
        inspect_stored_path(
            chunk_path(
                hash
            ),
            "inspect a chunk path"
        );

    return (
        path_state ==
        StoredPathState::regular_file
    );
}

std::vector<std::uint8_t>
ChunkStore::load(
    const std::string& hash
) const
{
    validate_hash(
        hash
    );

    const std::filesystem::path path =
        chunk_path(
            hash
        );

    const StoredPathState path_state =
        inspect_stored_path(
            path,
            "inspect a chunk path"
        );

    if (
        path_state ==
        StoredPathState::missing
    )
    {
        throw std::runtime_error(
            "Chunk does not exist: "
            + hash
        );
    }

    if (
        path_state ==
        StoredPathState::other
    )
    {
        throw std::runtime_error(
            "Chunk path is not a regular file: "
            + hash
        );
    }

    return read_and_verify_chunk_once(
        path,
        hash
    );
}

const std::filesystem::path&
ChunkStore::root_directory() const noexcept
{
    return root_directory_;
}

std::filesystem::path
ChunkStore::chunk_path(
    const std::string& hash
) const
{
    validate_hash(
        hash
    );

    return chunks_directory_
        / hash.substr(
            0,
            2
        )
        / hash.substr(
            2
        );
}

void ChunkStore::validate_hash(
    const std::string& hash
)
{
    constexpr std::size_t sha256_hex_length =
        64;

    if (
        hash.size() !=
        sha256_hex_length
    )
    {
        throw std::invalid_argument(
            "SHA-256 hash must contain "
            "64 hexadecimal characters."
        );
    }

    const bool is_valid =
        std::all_of(
            hash.begin(),
            hash.end(),
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
            "SHA-256 hash must contain only "
            "lowercase hexadecimal characters."
        );
    }
}

}