#include "nexusfs/storage/file_manifest.hpp"

#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace nexusfs::storage
{

namespace
{

bool is_valid_sha256_hash(
    const std::string& hash
)
{
    constexpr std::size_t sha256_hex_length = 64;

    if (hash.size() != sha256_hex_length)
    {
        return false;
    }

    for (const char character : hash)
    {
        const bool is_decimal_digit =
            character >= '0' &&
            character <= '9';

        const bool is_lowercase_hexadecimal =
            character >= 'a' &&
            character <= 'f';

        if (
            !is_decimal_digit &&
            !is_lowercase_hexadecimal
        )
        {
            return false;
        }
    }

    return true;
}

void validate_original_filename(
    const std::string& original_filename
)
{
    if (original_filename.empty())
    {
        throw std::invalid_argument(
            "Manifest filename cannot be empty."
        );
    }

    if (
        original_filename.find('\0') !=
        std::string::npos
    )
    {
        throw std::invalid_argument(
            "Manifest filename cannot contain a null character."
        );
    }

    const std::filesystem::path filename_path{
        original_filename
    };

    if (
        filename_path.has_root_name() ||
        filename_path.has_root_directory() ||
        filename_path.has_parent_path() ||
        filename_path.filename() != filename_path
    )
    {
        throw std::invalid_argument(
            "Manifest filename must not contain a directory path."
        );
    }

    if (
        original_filename == "." ||
        original_filename == ".."
    )
    {
        throw std::invalid_argument(
            "Manifest filename is not safe."
        );
    }
}

void validate_chunk_hashes(
    const std::vector<std::string>& chunk_hashes
)
{
    for (const std::string& hash : chunk_hashes)
    {
        if (!is_valid_sha256_hash(hash))
        {
            throw std::invalid_argument(
                "Manifest contains an invalid SHA-256 chunk hash."
            );
        }
    }
}

std::uint64_t convert_size_to_uint64(
    std::size_t value,
    const char* error_message
)
{
    if constexpr (
        sizeof(std::size_t) >
        sizeof(std::uint64_t)
    )
    {
        if (
            value >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max()
            )
        )
        {
            throw std::overflow_error(error_message);
        }
    }

    return static_cast<std::uint64_t>(value);
}

}

FileManifest FileManifest::create(
    const std::filesystem::path& file_path,
    std::size_t chunk_size,
    const std::vector<FileChunk>& chunks
)
{
    if (chunk_size == 0)
    {
        throw std::invalid_argument(
            "Manifest chunk size must be greater than zero."
        );
    }

    const std::string original_filename =
        file_path.filename().string();

    validate_original_filename(original_filename);

    std::vector<std::string> chunk_hashes;
    chunk_hashes.reserve(chunks.size());

    std::uint64_t calculated_file_size = 0;

    for (
        std::size_t position = 0;
        position < chunks.size();
        ++position
    )
    {
        const FileChunk& chunk = chunks[position];

        if (chunk.index != position)
        {
            throw std::runtime_error(
                "Chunks are not in contiguous index order."
            );
        }

        if (chunk.data.empty())
        {
            throw std::runtime_error(
                "A manifest cannot reference an empty chunk."
            );
        }

        if (chunk.data.size() > chunk_size)
        {
            throw std::runtime_error(
                "Chunk size exceeds the configured manifest chunk size."
            );
        }

        const bool is_final_chunk =
            position + 1 == chunks.size();

        if (
            !is_final_chunk &&
            chunk.data.size() != chunk_size
        )
        {
            throw std::runtime_error(
                "A non-final chunk does not have the configured chunk size."
            );
        }

        if (!is_valid_sha256_hash(chunk.hash))
        {
            throw std::runtime_error(
                "A chunk contains an invalid SHA-256 hash."
            );
        }

        const std::uint64_t current_chunk_size =
            convert_size_to_uint64(
                chunk.data.size(),
                "Chunk size exceeds the manifest size limit."
            );

        if (
            calculated_file_size >
            std::numeric_limits<std::uint64_t>::max()
                - current_chunk_size
        )
        {
            throw std::overflow_error(
                "File size exceeds the manifest size limit."
            );
        }

        calculated_file_size += current_chunk_size;
        chunk_hashes.push_back(chunk.hash);
    }

    std::error_code error;

    const std::uintmax_t actual_file_size =
        std::filesystem::file_size(
            file_path,
            error
        );

    if (error)
    {
        throw std::runtime_error(
            "Failed to determine original file size: "
            + error.message()
        );
    }

    if (
        actual_file_size >
        std::numeric_limits<std::uint64_t>::max()
    )
    {
        throw std::runtime_error(
            "Original file is too large for the manifest format."
        );
    }

    if (
        calculated_file_size !=
        static_cast<std::uint64_t>(actual_file_size)
    )
    {
        throw std::runtime_error(
            "Chunk data size does not match the original file size."
        );
    }

    return FileManifest{
        original_filename,
        calculated_file_size,
        chunk_size,
        std::move(chunk_hashes)
    };
}

FileManifest FileManifest::restore(
    std::string original_filename,
    std::uint64_t file_size,
    std::size_t chunk_size,
    std::vector<std::string> chunk_hashes
)
{
    validate_original_filename(original_filename);

    if (chunk_size == 0)
    {
        throw std::invalid_argument(
            "Restored manifest chunk size must be greater than zero."
        );
    }

    validate_chunk_hashes(chunk_hashes);

    const std::uint64_t encoded_chunk_size =
        convert_size_to_uint64(
            chunk_size,
            "Restored chunk size exceeds the manifest format limit."
        );

    const std::uint64_t encoded_chunk_count =
        convert_size_to_uint64(
            chunk_hashes.size(),
            "Restored chunk count exceeds the manifest format limit."
        );

    if (file_size == 0)
    {
        if (!chunk_hashes.empty())
        {
            throw std::invalid_argument(
                "An empty file manifest cannot reference chunks."
            );
        }
    }
    else
    {
        const std::uint64_t expected_chunk_count =
            ((file_size - 1) / encoded_chunk_size) + 1;

        if (encoded_chunk_count != expected_chunk_count)
        {
            throw std::invalid_argument(
                "Manifest chunk count is inconsistent with its file size."
            );
        }
    }

    return FileManifest{
        std::move(original_filename),
        file_size,
        chunk_size,
        std::move(chunk_hashes)
    };
}

FileManifest::FileManifest(
    std::string original_filename,
    std::uint64_t file_size,
    std::size_t chunk_size,
    std::vector<std::string> chunk_hashes
)
    : original_filename_{
          std::move(original_filename)
      },
      file_size_{file_size},
      chunk_size_{chunk_size},
      chunk_hashes_{std::move(chunk_hashes)}
{
}

std::uint32_t
FileManifest::format_version() const noexcept
{
    return current_format_version;
}

const std::string&
FileManifest::original_filename() const noexcept
{
    return original_filename_;
}

std::uint64_t
FileManifest::file_size() const noexcept
{
    return file_size_;
}

std::size_t
FileManifest::chunk_size() const noexcept
{
    return chunk_size_;
}

std::size_t
FileManifest::chunk_count() const noexcept
{
    return chunk_hashes_.size();
}

const std::vector<std::string>&
FileManifest::chunk_hashes() const noexcept
{
    return chunk_hashes_;
}

}