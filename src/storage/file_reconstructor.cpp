#include "nexusfs/storage/file_reconstructor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

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
                std::numeric_limits<
                    std::uint64_t
                >::max()
            )
        )
        {
            throw std::overflow_error(
                error_message
            );
        }
    }

    return static_cast<std::uint64_t>(
        value
    );
}

std::filesystem::path make_temporary_path(
    const std::filesystem::path& output_path
)
{
    /*
     * The atomic sequence prevents collisions when concurrent
     * reconstructions observe the same clock timestamp.
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
        output_path;

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

void require_output_path_absent(
    const std::filesystem::path& output_path
)
{
    std::error_code existence_error;

    const bool output_exists =
        std::filesystem::exists(
            output_path,
            existence_error
        );

    if (existence_error)
    {
        throw std::runtime_error(
            "Failed to inspect reconstruction output path: "
            + existence_error.message()
        );
    }

    if (output_exists)
    {
        throw std::runtime_error(
            "Reconstruction output path already exists: "
            + output_path.string()
        );
    }
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

    require_output_path_absent(
        output_path
    );

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

    const std::filesystem::path temporary_path =
        make_temporary_path(
            output_path
        );

    std::ofstream output{
        temporary_path,
        std::ios::binary
            | std::ios::trunc
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
                "Manifest chunk size exceeds "
                "the reconstruction limit."
            );

        const auto& chunk_hashes =
            manifest.chunk_hashes();

        for (
            std::size_t index = 0;
            index < chunk_hashes.size();
            ++index
        )
        {
            if (
                bytes_written >
                manifest.file_size()
            )
            {
                throw std::runtime_error(
                    "Reconstructed byte count exceeds "
                    "the manifest file size."
                );
            }

            const std::string& chunk_hash =
                chunk_hashes[index];

            const auto chunk_data =
                chunk_store.load(
                    chunk_hash
                );

            const std::uint64_t remaining_bytes =
                manifest.file_size()
                - bytes_written;

            const std::uint64_t expected_chunk_size =
                std::min(
                    configured_chunk_size,
                    remaining_bytes
                );

            const std::uint64_t actual_chunk_size =
                convert_size_to_uint64(
                    chunk_data.size(),
                    "Loaded chunk size exceeds "
                    "the reconstruction limit."
                );

            if (
                actual_chunk_size !=
                expected_chunk_size
            )
            {
                throw std::runtime_error(
                    "Stored chunk size is inconsistent "
                    "with the manifest at chunk index "
                    + std::to_string(index)
                    + "."
                );
            }

            if (
                chunk_data.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<
                        std::streamsize
                    >::max()
                )
            )
            {
                throw std::runtime_error(
                    "Chunk is too large for "
                    "stream-based reconstruction."
                );
            }

            if (!chunk_data.empty())
            {
                output.write(
                    reinterpret_cast<
                        const char*
                    >(
                        chunk_data.data()
                    ),
                    static_cast<std::streamsize>(
                        chunk_data.size()
                    )
                );

                if (!output)
                {
                    throw std::runtime_error(
                        "Failed while writing reconstructed "
                        "chunk "
                        + std::to_string(index)
                        + "."
                    );
                }
            }

            bytes_written +=
                actual_chunk_size;

            ++chunks_loaded;
        }

        if (
            chunks_loaded !=
            manifest.chunk_count()
        )
        {
            throw std::runtime_error(
                "Loaded chunk count does not match "
                "the manifest."
            );
        }

        if (
            bytes_written !=
            manifest.file_size()
        )
        {
            throw std::runtime_error(
                "Reconstructed file size does not match "
                "the manifest."
            );
        }

        output.flush();

        if (!output)
        {
            throw std::runtime_error(
                "Failed while flushing "
                "the reconstructed file."
            );
        }

        output.close();

        if (!output)
        {
            throw std::runtime_error(
                "Failed while closing "
                "the reconstructed file."
            );
        }

        std::error_code temporary_size_error;

        const std::uintmax_t temporary_file_size =
            std::filesystem::file_size(
                temporary_path,
                temporary_size_error
            );

        if (temporary_size_error)
        {
            throw std::runtime_error(
                "Failed to determine reconstructed "
                "temporary-file size: "
                + temporary_size_error.message()
            );
        }

        if (
            temporary_file_size !=
            static_cast<std::uintmax_t>(
                manifest.file_size()
            )
        )
        {
            throw std::runtime_error(
                "Temporary reconstructed file size "
                "does not match the manifest."
            );
        }

        /*
         * The service-level per-output-path lock prevents
         * concurrent NexusFS restorations from racing here.
         * Rechecking also protects against an output created
         * during reconstruction by another local operation.
         */
        require_output_path_absent(
            output_path
        );

        std::error_code rename_error;

        std::filesystem::rename(
            temporary_path,
            output_path,
            rename_error
        );

        if (rename_error)
        {
            std::error_code existence_error;

            const bool output_exists =
                std::filesystem::exists(
                    output_path,
                    existence_error
                );

            if (
                !existence_error
                && output_exists
            )
            {
                throw std::runtime_error(
                    "Reconstruction output path "
                    "already exists: "
                    + output_path.string()
                );
            }

            throw std::runtime_error(
                "Failed to finalize the reconstructed file: "
                + rename_error.message()
            );
        }
    }
    catch (...)
    {
        output.close();

        remove_temporary_file(
            temporary_path
        );

        throw;
    }

    return ReconstructionResult{
        bytes_written,
        chunks_loaded
    };
}

}