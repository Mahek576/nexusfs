#ifndef NEXUSFS_STORAGE_MANIFEST_STORE_HPP
#define NEXUSFS_STORAGE_MANIFEST_STORE_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nexusfs::storage
{

enum class ManifestStoreResult
{
    stored,
    already_exists
};

class ManifestStore
{
public:
    explicit ManifestStore(
        std::filesystem::path root_directory
    );

    [[nodiscard]] ManifestStoreResult store(
        const std::string& manifest_id,
        const std::vector<std::uint8_t>& encoded_manifest
    );

    [[nodiscard]] bool contains(
        const std::string& manifest_id
    ) const;

    [[nodiscard]] std::vector<std::uint8_t> load(
        const std::string& manifest_id
    ) const;

    [[nodiscard]] const std::filesystem::path&
    root_directory() const noexcept;

private:
    [[nodiscard]] std::filesystem::path manifest_path(
        const std::string& manifest_id
    ) const;

    static void validate_manifest_id(
        const std::string& manifest_id
    );

    std::filesystem::path root_directory_;
    std::filesystem::path manifests_directory_;
};

}

#endif