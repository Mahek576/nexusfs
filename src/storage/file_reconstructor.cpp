#include "nexusfs/storage/file_reconstructor.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace nexusfs::storage
{

namespace
{

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

ReconstructionResult FileReconstructor::reconstruct(
    const FileManifest& manifest,
    const ChunkStore& chunk_store,
    const std::filesystem::path& output_path
)
{
    if (output_path.empty())
    {
        throw std::invalid_argument(
            "Reconstruction output path cannot be empty."
        );
    }

    if (manifest.chunk_size() == 0)
    {
        throw std::runtime_error(
            "Cannot reconstruct a file with a zero chunk size."
        );
    }

    if (std::filesystem::exists(output_path))
    {
        throw std::runtime_error(
            "Reconstruction output path already exists: "
            + output_path.string()
        );
    }

    const std::filesystem::path parent_directory =
        output_path.parent_path();

    if (!parent_directory.empty())
    {
        std::error_code directory_error;

        std::filesystem::create_directories(
            parent_directory,
            directory_error
        );

        if (directory_error)
        {
            throw std::runtime_error(
                "Failed to create reconstruction directory: "
                + directory_error.message()
            );
        }
    }

    const auto timestamp =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    std::filesystem::path temporary_path =
        output_path;

    temporary_path +=
        ".tmp."
        + std::to_string(timestamp);

    std::ofstream output{
        temporary_path,
        std::ios::binary | std::ios::trunc
    };

    if (!output.is_open())
    {
        throw std::runtime_error(
            "Failed to create temporary reconstructed file: "
            + temporary_path.string()
        );
    }

    std::uint64_t bytes_written = 0;
    std::size_t chunks_loaded = 0;

    try
    {
        const std::uint64_t configured_chunk_size =
            convert_size_to_uint64(
                manifest.chunk_size(),
                "Manifest chunk size exceeds the reconstruction limit."
            );

        for (
            std::size_t index = 0;
            index < manifest.chunk_hashes().size();
            ++index
        )
        {
            if (bytes_written > manifest.file_size())
            {
                throw std::runtime_error(
                    "Reconstructed byte count exceeds the manifest file size."
                );
            }

            const std::string& chunk_hash =
                manifest.chunk_hashes()[index];

            const auto chunk_data =
                chunk_store.load(chunk_hash);

            const std::uint64_t remaining_bytes =
                manifest.file_size() - bytes_written;

            const std::uint64_t expected_chunk_size =
                std::min(
                    configured_chunk_size,
                    remaining_bytes
                );

            const std::uint64_t actual_chunk_size =
                convert_size_to_uint64(
                    chunk_data.size(),
                    "Loaded chunk size exceeds the reconstruction limit."
                );

            if (actual_chunk_size != expected_chunk_size)
            {
                throw std::runtime_error(
                    "Stored chunk size is inconsistent with the manifest "
                    "at chunk index "
                    + std::to_string(index)
                    + "."
                );
            }

            if (
                chunk_data.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<std::streamsize>::max()
                )
            )
            {
                throw std::runtime_error(
                    "Chunk is too large for stream-based reconstruction."
                );
            }

            if (!chunk_data.empty())
            {
                output.write(
                    reinterpret_cast<const char*>(
                        chunk_data.data()
                    ),
                    static_cast<std::streamsize>(
                        chunk_data.size()
                    )
                );

                if (!output)
                {
                    throw std::runtime_error(
                        "Failed while writing reconstructed chunk "
                        + std::to_string(index)
                        + "."
                    );
                }
            }

            bytes_written += actual_chunk_size;
            ++chunks_loaded;
        }

        if (chunks_loaded != manifest.chunk_count())
        {
            throw std::runtime_error(
                "Loaded chunk count does not match the manifest."
            );
        }

        if (bytes_written != manifest.file_size())
        {
            throw std::runtime_error(
                "Reconstructed file size does not match the manifest."
            );
        }

        output.flush();

        if (!output)
        {
            throw std::runtime_error(
                "Failed while flushing the reconstructed file."
            );
        }

        output.close();

        if (!output)
        {
            throw std::runtime_error(
                "Failed while closing the reconstructed file."
            );
        }

        std::error_code rename_error;

        std::filesystem::rename(
            temporary_path,
            output_path,
            rename_error
        );

        if (rename_error)
        {
            throw std::runtime_error(
                "Failed to finalize the reconstructed file: "
                + rename_error.message()
            );
        }
    }
    catch (...)
    {
        output.close();

        std::error_code cleanup_error;

        std::filesystem::remove(
            temporary_path,
            cleanup_error
        );

        throw;
    }

    return ReconstructionResult{
        bytes_written,
        chunks_loaded
    };
}

}