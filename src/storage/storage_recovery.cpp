#include "nexusfs/storage/storage_recovery.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace nexusfs::storage
{

namespace
{

bool contains_only_decimal_digits(
    std::string_view value
) noexcept
{
    if (value.empty())
    {
        return false;
    }

    return std::all_of(
        value.begin(),
        value.end(),
        [](char character)
        {
            return (
                character >= '0'
                && character <= '9'
            );
        }
    );
}

bool is_nexusfs_temporary_filename(
    const std::filesystem::path& path
) noexcept
{
    const std::string filename =
        path.filename().string();

    constexpr std::string_view marker{
        ".tmp."
    };

    const std::size_t marker_position =
        filename.rfind(
            marker
        );

    if (
        marker_position
            == std::string::npos
        || marker_position == 0
    )
    {
        return false;
    }

    const std::string_view suffix{
        filename.data()
            + marker_position
            + marker.size(),
        filename.size()
            - marker_position
            - marker.size()
    };

    const std::size_t first_separator =
        suffix.find('.');

    if (
        first_separator
        == std::string_view::npos
    )
    {
        return false;
    }

    const std::size_t second_separator =
        suffix.find(
            '.',
            first_separator + 1
        );

    if (
        second_separator
        == std::string_view::npos
    )
    {
        return false;
    }

    if (
        suffix.find(
            '.',
            second_separator + 1
        )
        != std::string_view::npos
    )
    {
        return false;
    }

    const std::string_view timestamp =
        suffix.substr(
            0,
            first_separator
        );

    const std::string_view thread_token =
        suffix.substr(
            first_separator + 1,
            second_separator
                - first_separator
                - 1
        );

    const std::string_view sequence =
        suffix.substr(
            second_separator + 1
        );

    return (
        contains_only_decimal_digits(
            timestamp
        )
        && contains_only_decimal_digits(
            thread_token
        )
        && contains_only_decimal_digits(
            sequence
        )
    );
}

bool is_missing_path_error(
    const std::error_code& error
) noexcept
{
    return (
        error
            == std::errc::
                no_such_file_or_directory
        || error
            == std::errc::
                not_a_directory
    );
}

void scan_managed_directory(
    const std::filesystem::path& directory,
    StorageRecoveryReport& report
)
{
    std::error_code status_error;

    const std::filesystem::file_status
        directory_status =
            std::filesystem::symlink_status(
                directory,
                status_error
            );

    if (status_error)
    {
        if (
            is_missing_path_error(
                status_error
            )
        )
        {
            return;
        }

        throw std::runtime_error(
            "Failed to inspect recovery directory "
            + directory.string()
            + ": "
            + status_error.message()
        );
    }

    if (
        !std::filesystem::exists(
            directory_status
        )
    )
    {
        return;
    }

    if (
        !std::filesystem::is_directory(
            directory_status
        )
    )
    {
        throw std::runtime_error(
            "NexusFS recovery path is not "
            "a directory: "
            + directory.string()
        );
    }

    std::error_code iterator_error;

    std::filesystem::recursive_directory_iterator
        iterator{
            directory,
            std::filesystem::
                directory_options::none,
            iterator_error
        };

    if (iterator_error)
    {
        throw std::runtime_error(
            "Failed to begin storage recovery scan: "
            + iterator_error.message()
        );
    }

    const std::filesystem::
        recursive_directory_iterator end;

    while (iterator != end)
    {
        const std::filesystem::directory_entry
            entry =
                *iterator;

        ++report.entries_scanned;

        if (
            is_nexusfs_temporary_filename(
                entry.path()
            )
        )
        {
            ++report.temporary_entries_found;

            std::error_code entry_status_error;

            const std::filesystem::file_status
                entry_status =
                    entry.symlink_status(
                        entry_status_error
                    );

            if (entry_status_error)
            {
                throw std::runtime_error(
                    "Failed to inspect recovery candidate "
                    + entry.path().string()
                    + ": "
                    + entry_status_error.message()
                );
            }

            if (
                std::filesystem::is_regular_file(
                    entry_status
                )
            )
            {
                std::error_code removal_error;

                const bool removed =
                    std::filesystem::remove(
                        entry.path(),
                        removal_error
                    );

                if (
                    removal_error
                    || !removed
                )
                {
                    throw std::runtime_error(
                        "Failed to remove abandoned "
                        "temporary file "
                        + entry.path().string()
                        + (
                            removal_error
                            ? ": "
                              + removal_error.message()
                            : "."
                        )
                    );
                }

                ++report.temporary_files_removed;
            }
            else
            {
                ++report
                    .non_regular_entries_preserved;
            }
        }

        iterator.increment(
            iterator_error
        );

        if (iterator_error)
        {
            throw std::runtime_error(
                "Failed while scanning NexusFS "
                "recovery directories: "
                + iterator_error.message()
            );
        }
    }
}

}

StorageRecoveryReport recover_storage_root(
    const std::filesystem::path& storage_root
)
{
    if (storage_root.empty())
    {
        throw std::invalid_argument(
            "Storage recovery root cannot be empty."
        );
    }

    std::error_code root_status_error;

    const std::filesystem::file_status
        root_status =
            std::filesystem::symlink_status(
                storage_root,
                root_status_error
            );

    if (root_status_error)
    {
        if (
            is_missing_path_error(
                root_status_error
            )
        )
        {
            return {};
        }

        throw std::runtime_error(
            "Failed to inspect storage recovery root: "
            + root_status_error.message()
        );
    }

    if (
        !std::filesystem::exists(
            root_status
        )
    )
    {
        return {};
    }

    if (
        !std::filesystem::is_directory(
            root_status
        )
    )
    {
        throw std::runtime_error(
            "Storage recovery root is not "
            "a directory: "
            + storage_root.string()
        );
    }

    StorageRecoveryReport report;

    scan_managed_directory(
        storage_root / "chunks",
        report
    );

    scan_managed_directory(
        storage_root / "manifests",
        report
    );

    return report;
}

}
