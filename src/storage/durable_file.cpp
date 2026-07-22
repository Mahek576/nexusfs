#include "nexusfs/storage/durable_file.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

#else

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#endif

namespace nexusfs::storage
{

namespace
{

std::filesystem::path normalized_parent(
    const std::filesystem::path& path
)
{
    std::filesystem::path parent =
        path.parent_path();

    if (parent.empty())
    {
        parent = ".";
    }

    std::error_code absolute_error;

    const std::filesystem::path absolute_parent =
        std::filesystem::absolute(
            parent,
            absolute_error
        );

    if (absolute_error)
    {
        throw std::runtime_error(
            "Failed to resolve publication directory: "
            + absolute_error.message()
        );
    }

    return absolute_parent.lexically_normal();
}

void validate_publication_paths(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& destination_path
)
{
    if (temporary_path.empty())
    {
        throw std::invalid_argument(
            "Temporary publication path cannot be empty."
        );
    }

    if (destination_path.empty())
    {
        throw std::invalid_argument(
            "Destination publication path cannot be empty."
        );
    }

    const std::filesystem::path
        normalized_temporary_path =
            std::filesystem::absolute(
                temporary_path
            ).lexically_normal();

    const std::filesystem::path
        normalized_destination_path =
            std::filesystem::absolute(
                destination_path
            ).lexically_normal();

    if (
        normalized_temporary_path ==
        normalized_destination_path
    )
    {
        throw std::invalid_argument(
            "Temporary and destination paths must differ."
        );
    }

    if (
        normalized_parent(
            temporary_path
        )
        !=
        normalized_parent(
            destination_path
        )
    )
    {
        throw std::invalid_argument(
            "Durable publication requires temporary and "
            "destination files to share one directory."
        );
    }
}

#ifdef _WIN32

std::error_code windows_error_code(
    DWORD error
)
{
    return std::error_code{
        static_cast<int>(
            error
        ),
        std::system_category()
    };
}

bool windows_path_exists(
    const std::filesystem::path& path
) noexcept
{
    return (
        GetFileAttributesW(
            path.c_str()
        )
        != INVALID_FILE_ATTRIBUTES
    );
}

#else

class PosixFileDescriptor final
{
public:
    explicit PosixFileDescriptor(
        int descriptor
    ) noexcept
        : descriptor_{
              descriptor
          }
    {
    }

    PosixFileDescriptor(
        const PosixFileDescriptor&
    ) = delete;

    PosixFileDescriptor& operator=(
        const PosixFileDescriptor&
    ) = delete;

    ~PosixFileDescriptor()
    {
        if (descriptor_ >= 0)
        {
            ::close(
                descriptor_
            );
        }
    }

    [[nodiscard]] int get() const noexcept
    {
        return descriptor_;
    }

    [[nodiscard]] int release() noexcept
    {
        const int descriptor =
            descriptor_;

        descriptor_ = -1;

        return descriptor;
    }

private:
    int descriptor_;
};

int close_descriptor(
    PosixFileDescriptor& descriptor,
    const std::string& operation
)
{
    const int raw_descriptor =
        descriptor.release();

    if (
        ::close(
            raw_descriptor
        ) != 0
    )
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            operation
        );
    }

    return 0;
}

void flush_directory_to_disk(
    const std::filesystem::path& directory_path
)
{
    int open_flags =
        O_RDONLY;

#ifdef O_DIRECTORY
    open_flags |= O_DIRECTORY;
#endif

#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif

    const int raw_descriptor =
        ::open(
            directory_path.c_str(),
            open_flags
        );

    if (raw_descriptor < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to open directory for durable flush: "
            + directory_path.string()
        );
    }

    PosixFileDescriptor descriptor{
        raw_descriptor
    };

    if (
        ::fsync(
            descriptor.get()
        ) != 0
    )
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to flush publication directory: "
            + directory_path.string()
        );
    }

    close_descriptor(
        descriptor,
        "Failed to close publication directory: "
        + directory_path.string()
    );
}

#endif

}

void flush_file_to_disk(
    const std::filesystem::path& file_path
)
{
    if (file_path.empty())
    {
        throw std::invalid_argument(
            "File flush path cannot be empty."
        );
    }

#ifdef _WIN32

    const HANDLE file_handle =
        CreateFileW(
            file_path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ
                | FILE_SHARE_WRITE
                | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

    if (
        file_handle ==
        INVALID_HANDLE_VALUE
    )
    {
        const DWORD error =
            GetLastError();

        throw std::system_error(
            windows_error_code(
                error
            ),
            "Failed to open file for durable flush: "
            + file_path.string()
        );
    }

    if (
        FlushFileBuffers(
            file_handle
        ) == 0
    )
    {
        const DWORD flush_error =
            GetLastError();

        CloseHandle(
            file_handle
        );

        throw std::system_error(
            windows_error_code(
                flush_error
            ),
            "Failed to flush file data: "
            + file_path.string()
        );
    }

    if (
        CloseHandle(
            file_handle
        ) == 0
    )
    {
        const DWORD close_error =
            GetLastError();

        throw std::system_error(
            windows_error_code(
                close_error
            ),
            "Failed to close flushed file: "
            + file_path.string()
        );
    }

#else

    int open_flags =
        O_RDWR;

#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif

    const int raw_descriptor =
        ::open(
            file_path.c_str(),
            open_flags
        );

    if (raw_descriptor < 0)
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to open file for durable flush: "
            + file_path.string()
        );
    }

    PosixFileDescriptor descriptor{
        raw_descriptor
    };

    if (
        ::fsync(
            descriptor.get()
        ) != 0
    )
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to flush file data: "
            + file_path.string()
        );
    }

    close_descriptor(
        descriptor,
        "Failed to close flushed file: "
        + file_path.string()
    );

#endif
}

DurablePublishResult publish_file_durably(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& destination_path
)
{
    validate_publication_paths(
        temporary_path,
        destination_path
    );

    flush_file_to_disk(
        temporary_path
    );

#ifdef _WIN32

    const BOOL moved =
        MoveFileExW(
            temporary_path.c_str(),
            destination_path.c_str(),
            MOVEFILE_WRITE_THROUGH
        );

    if (moved != 0)
    {
        return DurablePublishResult::published;
    }

    const DWORD move_error =
        GetLastError();

    if (
        move_error == ERROR_ALREADY_EXISTS
        || move_error == ERROR_FILE_EXISTS
        || windows_path_exists(
            destination_path
        )
    )
    {
        return DurablePublishResult::
            destination_exists;
    }

    throw std::system_error(
        windows_error_code(
            move_error
        ),
        "Failed to publish file durably from "
        + temporary_path.string()
        + " to "
        + destination_path.string()
    );

#else

    /*
     * link() creates the destination atomically and fails with
     * EEXIST rather than replacing existing content. Because both
     * names are in the same directory, they reference one inode.
     */
    if (
        ::link(
            temporary_path.c_str(),
            destination_path.c_str()
        ) != 0
    )
    {
        if (errno == EEXIST)
        {
            return DurablePublishResult::
                destination_exists;
        }

        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to publish file durably from "
            + temporary_path.string()
            + " to "
            + destination_path.string()
        );
    }

    const std::filesystem::path directory =
        normalized_parent(
            destination_path
        );

    /*
     * Persist creation of the destination name before removing the
     * temporary name. A crash between these operations can leave
     * both names, which startup recovery can safely clean.
     */
    flush_directory_to_disk(
        directory
    );

    if (
        ::unlink(
            temporary_path.c_str()
        ) != 0
    )
    {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Published the destination but failed to remove "
            "its temporary name: "
            + temporary_path.string()
        );
    }

    flush_directory_to_disk(
        directory
    );

    return DurablePublishResult::published;

#endif
}

}