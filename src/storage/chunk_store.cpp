#include "nexusfs/storage/chunk_store.hpp"

#include "nexusfs/storage/sha256_hasher.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace nexusfs::storage
{

ChunkStore::ChunkStore(std::filesystem::path root_directory)
    : root_directory_{std::move(root_directory)},
      chunks_directory_{root_directory_ / "chunks"}
{
    if (root_directory_.empty())
    {
        throw std::invalid_argument(
            "Chunk store root directory cannot be empty."
        );
    }

    std::error_code error;

    std::filesystem::create_directories(
        chunks_directory_,
        error
    );

    if (error)
    {
        throw std::runtime_error(
            "Failed to create chunk storage directory: "
            + error.message()
        );
    }
}

StoreResult ChunkStore::store(const FileChunk& chunk)
{
    validate_hash(chunk.hash);

    const std::string calculated_hash =
        Sha256Hasher::hash(chunk.data);

    if (calculated_hash != chunk.hash)
    {
        throw std::runtime_error(
            "Chunk data does not match its SHA-256 hash."
        );
    }

    const std::filesystem::path final_path =
        chunk_path(chunk.hash);

    if (std::filesystem::exists(final_path))
    {
        return StoreResult::already_exists;
    }

    const std::filesystem::path parent_directory =
        final_path.parent_path();

    std::error_code error;

    std::filesystem::create_directories(
        parent_directory,
        error
    );

    if (error)
    {
        throw std::runtime_error(
            "Failed to create chunk directory: "
            + error.message()
        );
    }

    const auto timestamp =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    std::filesystem::path temporary_path = final_path;

    temporary_path +=
        ".tmp."
        + std::to_string(timestamp);

    {
        std::ofstream output{
            temporary_path,
            std::ios::binary | std::ios::trunc
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
            output.close();

            std::filesystem::remove(
                temporary_path,
                error
            );

            throw std::runtime_error(
                "Failed while writing chunk data."
            );
        }
    }

    std::filesystem::rename(
        temporary_path,
        final_path,
        error
    );

    if (error)
    {
        if (std::filesystem::exists(final_path))
        {
            std::filesystem::remove(
                temporary_path,
                error
            );

            return StoreResult::already_exists;
        }

        std::filesystem::remove(
            temporary_path,
            error
        );

        throw std::runtime_error(
            "Failed to finalize chunk file: "
            + error.message()
        );
    }

    return StoreResult::stored;
}

bool ChunkStore::contains(
    const std::string& hash
) const
{
    validate_hash(hash);

    return std::filesystem::is_regular_file(
        chunk_path(hash)
    );
}

std::vector<std::uint8_t> ChunkStore::load(
    const std::string& hash
) const
{
    validate_hash(hash);

    const std::filesystem::path path =
        chunk_path(hash);

    if (!std::filesystem::is_regular_file(path))
    {
        throw std::runtime_error(
            "Chunk does not exist: " + hash
        );
    }

    std::error_code error;

    const std::uintmax_t file_size =
        std::filesystem::file_size(path, error);

    if (error)
    {
        throw std::runtime_error(
            "Failed to determine chunk size: "
            + error.message()
        );
    }

    if (
        file_size >
        static_cast<std::uintmax_t>(
            std::numeric_limits<std::size_t>::max()
        )
    )
    {
        throw std::runtime_error(
            "Chunk is too large to load into memory."
        );
    }

    std::ifstream input{
        path,
        std::ios::binary
    };

    if (!input.is_open())
    {
        throw std::runtime_error(
            "Failed to open chunk: " + hash
        );
    }

    std::vector<std::uint8_t> data(
        static_cast<std::size_t>(file_size)
    );

    if (!data.empty())
    {
        input.read(
            reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(
                data.size()
            )
        );

        if (!input)
        {
            throw std::runtime_error(
                "Failed while reading chunk: " + hash
            );
        }
    }

    const std::string calculated_hash =
        Sha256Hasher::hash(data);

    if (calculated_hash != hash)
    {
        throw std::runtime_error(
            "Chunk integrity verification failed: "
            + hash
        );
    }

    return data;
}

const std::filesystem::path&
ChunkStore::root_directory() const noexcept
{
    return root_directory_;
}

std::filesystem::path ChunkStore::chunk_path(
    const std::string& hash
) const
{
    validate_hash(hash);

    return chunks_directory_
        / hash.substr(0, 2)
        / hash.substr(2);
}

void ChunkStore::validate_hash(
    const std::string& hash
)
{
    constexpr std::size_t sha256_hex_length = 64;

    if (hash.size() != sha256_hex_length)
    {
        throw std::invalid_argument(
            "SHA-256 hash must contain 64 hexadecimal characters."
        );
    }

    const bool is_valid = std::all_of(
        hash.begin(),
        hash.end(),
        [](char character)
        {
            const auto value =
                static_cast<unsigned char>(character);

            return std::isxdigit(value) != 0;
        }
    );

    if (!is_valid)
    {
        throw std::invalid_argument(
            "SHA-256 hash contains invalid characters."
        );
    }
}

}