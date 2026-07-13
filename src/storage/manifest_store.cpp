#include "nexusfs/storage/manifest_store.hpp"

#include "nexusfs/storage/sha256_hasher.hpp"

#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace nexusfs::storage
{

ManifestStore::ManifestStore(
    std::filesystem::path root_directory
)
    : root_directory_{std::move(root_directory)},
      manifests_directory_{root_directory_ / "manifests"}
{
    if (root_directory_.empty())
    {
        throw std::invalid_argument(
            "Manifest store root directory cannot be empty."
        );
    }

    std::error_code error;

    std::filesystem::create_directories(
        manifests_directory_,
        error
    );

    if (error)
    {
        throw std::runtime_error(
            "Failed to create manifest storage directory: "
            + error.message()
        );
    }
}

ManifestStoreResult ManifestStore::store(
    const std::string& manifest_id,
    const std::vector<std::uint8_t>& encoded_manifest
)
{
    validate_manifest_id(manifest_id);

    if (encoded_manifest.empty())
    {
        throw std::invalid_argument(
            "Encoded manifest cannot be empty."
        );
    }

    const std::string calculated_id =
        Sha256Hasher::hash(encoded_manifest);

    if (calculated_id != manifest_id)
    {
        throw std::runtime_error(
            "Encoded manifest does not match its manifest ID."
        );
    }

    const std::filesystem::path final_path =
        manifest_path(manifest_id);

    if (std::filesystem::exists(final_path))
    {
       (void)load(manifest_id);

       return ManifestStoreResult::already_exists;
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
            "Failed to create manifest shard directory: "
            + error.message()
        );
    }

    const auto timestamp =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    std::filesystem::path temporary_path =
        final_path;

    temporary_path +=
        ".tmp."
        + std::to_string(timestamp);

    if (
       encoded_manifest.size() >
       static_cast<std::size_t>(
       std::numeric_limits<std::streamsize>::max()
       )
    )
    {
        throw std::runtime_error(
        "Encoded manifest is too large for stream-based storage."
         );
    }
    {
        std::ofstream output{
            temporary_path,
            std::ios::binary | std::ios::trunc
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
            output.close();

            std::error_code cleanup_error;

            std::filesystem::remove(
                temporary_path,
                cleanup_error
            );

            throw std::runtime_error(
                "Failed while writing manifest data."
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
            std::error_code cleanup_error;

            std::filesystem::remove(
                temporary_path,
                cleanup_error
            );

            (void)load(manifest_id);

            return ManifestStoreResult::already_exists;
        }

        const std::string rename_error =
            error.message();

        std::error_code cleanup_error;

        std::filesystem::remove(
            temporary_path,
            cleanup_error
        );

        throw std::runtime_error(
            "Failed to finalize manifest file: "
            + rename_error
        );
    }

    return ManifestStoreResult::stored;
}

bool ManifestStore::contains(
    const std::string& manifest_id
) const
{
    validate_manifest_id(manifest_id);

    return std::filesystem::is_regular_file(
        manifest_path(manifest_id)
    );
}

std::vector<std::uint8_t> ManifestStore::load(
    const std::string& manifest_id
) const
{
    validate_manifest_id(manifest_id);

    const std::filesystem::path path =
        manifest_path(manifest_id);

    if (!std::filesystem::is_regular_file(path))
    {
        throw std::runtime_error(
            "Manifest does not exist: "
            + manifest_id
        );
    }

    std::error_code error;

    const std::uintmax_t file_size =
        std::filesystem::file_size(
            path,
            error
        );

    if (error)
    {
        throw std::runtime_error(
            "Failed to determine manifest size: "
            + error.message()
        );
    }

    if (file_size == 0)
    {
        throw std::runtime_error(
            "Stored manifest is empty: "
            + manifest_id
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
            "Manifest is too large to load into memory."
        );
    }

    if (
        file_size >
        static_cast<std::uintmax_t>(
            std::numeric_limits<std::streamsize>::max()
        )
    )
    {
        throw std::runtime_error(
            "Manifest is too large for stream-based loading."
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
            + manifest_id
        );
    }

    std::vector<std::uint8_t> encoded_manifest(
        static_cast<std::size_t>(file_size)
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
            + manifest_id
        );
    }

    const std::string calculated_id =
        Sha256Hasher::hash(encoded_manifest);

    if (calculated_id != manifest_id)
    {
        throw std::runtime_error(
            "Manifest integrity verification failed: "
            + manifest_id
        );
    }

    return encoded_manifest;
}

const std::filesystem::path&
ManifestStore::root_directory() const noexcept
{
    return root_directory_;
}

std::filesystem::path ManifestStore::manifest_path(
    const std::string& manifest_id
) const
{
    validate_manifest_id(manifest_id);

    return manifests_directory_
        / manifest_id.substr(0, 2)
        / manifest_id.substr(2);
}

void ManifestStore::validate_manifest_id(
    const std::string& manifest_id
)
{
    constexpr std::size_t sha256_hex_length = 64;

    if (manifest_id.size() != sha256_hex_length)
    {
        throw std::invalid_argument(
            "Manifest ID must contain 64 hexadecimal characters."
        );
    }

    for (const char character : manifest_id)
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
            throw std::invalid_argument(
                "Manifest ID contains invalid characters."
            );
        }
    }
}

}