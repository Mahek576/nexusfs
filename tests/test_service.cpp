#include "nexusfs/app/nexusfs_service.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        const auto timestamp =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();

        path_ =
            std::filesystem::temp_directory_path()
            / (
                "nexusfs-service-tests-"
                + std::to_string(timestamp)
            );

        std::error_code error;

        std::filesystem::create_directories(
            path_,
            error
        );

        if (error)
        {
            throw std::runtime_error(
                "Failed to create temporary service-test directory: "
                + error.message()
            );
        }
    }

    TemporaryDirectory(
        const TemporaryDirectory&
    ) = delete;

    TemporaryDirectory& operator=(
        const TemporaryDirectory&
    ) = delete;

    ~TemporaryDirectory()
    {
        std::error_code error;

        std::filesystem::remove_all(
            path_,
            error
        );
    }

    [[nodiscard]] const std::filesystem::path&
    path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void require_true(
    bool condition,
    const std::string& test_name
)
{
    if (!condition)
    {
        throw std::runtime_error(
            test_name + " failed."
        );
    }
}

template <typename Actual, typename Expected>
void require_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& test_name
)
{
    if (actual != expected)
    {
        throw std::runtime_error(
            test_name + " failed."
        );
    }
}

template <typename Function>
void require_throws(
    Function&& function,
    const std::string& test_name
)
{
    bool exception_thrown = false;

    try
    {
        std::forward<Function>(function)();
    }
    catch (const std::exception&)
    {
        exception_thrown = true;
    }

    if (!exception_thrown)
    {
        throw std::runtime_error(
            test_name
            + " failed because no exception was thrown."
        );
    }
}

std::vector<std::uint8_t> create_test_data(
    std::size_t size,
    std::size_t chunk_size
)
{
    std::vector<std::uint8_t> data;
    data.reserve(size);

    for (std::size_t index = 0; index < size; ++index)
    {
        const std::size_t chunk_index =
            index / chunk_size;

        const std::size_t position_in_chunk =
            index % chunk_size;

        const auto value =
            static_cast<std::uint8_t>(
                (
                    position_in_chunk * 37U
                    + chunk_index * 53U
                    + 11U
                )
                % 256U
            );

        data.push_back(value);
    }

    return data;
}

void write_binary_file(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& data
)
{
    const std::filesystem::path parent =
        path.parent_path();

    if (!parent.empty())
    {
        std::error_code directory_error;

        std::filesystem::create_directories(
            parent,
            directory_error
        );

        if (directory_error)
        {
            throw std::runtime_error(
                "Failed to create service-test file directory: "
                + directory_error.message()
            );
        }
    }

    if (
        data.size() >
        static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max()
        )
    )
    {
        throw std::runtime_error(
            "Service-test data is too large for stream writing."
        );
    }

    std::ofstream output{
        path,
        std::ios::binary | std::ios::trunc
    };

    if (!output.is_open())
    {
        throw std::runtime_error(
            "Failed to create service-test file: "
            + path.string()
        );
    }

    if (!data.empty())
    {
        output.write(
            reinterpret_cast<const char*>(
                data.data()
            ),
            static_cast<std::streamsize>(
                data.size()
            )
        );
    }

    output.flush();

    if (!output)
    {
        throw std::runtime_error(
            "Failed while writing service-test file: "
            + path.string()
        );
    }
}

std::vector<std::uint8_t> read_binary_file(
    const std::filesystem::path& path
)
{
    std::error_code error;

    const std::uintmax_t file_size =
        std::filesystem::file_size(
            path,
            error
        );

    if (error)
    {
        throw std::runtime_error(
            "Failed to determine service-test file size: "
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
            "Service-test file is too large to load."
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
            "Service-test file is too large for stream reading."
        );
    }

    std::ifstream input{
        path,
        std::ios::binary
    };

    if (!input.is_open())
    {
        throw std::runtime_error(
            "Failed to open service-test file: "
            + path.string()
        );
    }

    std::vector<std::uint8_t> data(
        static_cast<std::size_t>(file_size)
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
                "Failed while reading service-test file: "
                + path.string()
            );
        }
    }

    return data;
}

void test_service_constructor_validation()
{
    require_throws(
        []()
        {
            (void)nexusfs::app::NexusFsService{
                std::filesystem::path{},
                1024
            };
        },
        "Empty service storage-root rejection test"
    );

    require_throws(
        []()
        {
            (void)nexusfs::app::NexusFsService{
                "storage",
                0
            };
        },
        "Zero service chunk-size rejection test"
    );
}

void test_service_storage_workflow()
{
    constexpr std::size_t chunk_size = 1024;
    constexpr std::size_t file_size = 2500;

    TemporaryDirectory temporary_directory;

    const std::filesystem::path storage_root =
        temporary_directory.path()
        / "storage";

    const std::filesystem::path source_path =
        temporary_directory.path()
        / "source.bin";

    const std::filesystem::path restored_path =
        temporary_directory.path()
        / "restored"
        / "source.bin";

    const auto original_data =
        create_test_data(
            file_size,
            chunk_size
        );

    write_binary_file(
        source_path,
        original_data
    );

    const nexusfs::app::NexusFsService service{
        storage_root,
        chunk_size
    };

    require_equal(
        service.storage_root(),
        storage_root,
        "Service storage-root accessor test"
    );

    require_equal(
        service.default_chunk_size(),
        chunk_size,
        "Service chunk-size accessor test"
    );

    const auto first_store_result =
        service.store_file(source_path);

    require_equal(
        first_store_result.source_path,
        source_path,
        "Service store source-path test"
    );

    require_equal(
        first_store_result.original_filename,
        std::string{"source.bin"},
        "Service store filename test"
    );

    require_equal(
        first_store_result.file_size,
        static_cast<std::uint64_t>(file_size),
        "Service store file-size test"
    );

    require_equal(
        first_store_result.chunk_count,
        static_cast<std::size_t>(3),
        "Service store chunk-count test"
    );

    require_equal(
        first_store_result.chunks_stored,
        static_cast<std::size_t>(3),
        "Service first-store chunk count test"
    );

    require_equal(
        first_store_result.chunks_reused,
        static_cast<std::size_t>(0),
        "Service first-store reused count test"
    );

    require_equal(
        first_store_result.bytes_processed,
        static_cast<std::uint64_t>(file_size),
        "Service processed-byte count test"
    );

    require_true(
        first_store_result.encoded_manifest_size > 0,
        "Service encoded-manifest size test"
    );

    require_true(
        first_store_result.manifest_stored,
        "Service first manifest-storage result test"
    );

    require_equal(
        first_store_result.manifest_id.size(),
        static_cast<std::size_t>(64),
        "Service manifest-ID length test"
    );

    const auto second_store_result =
        service.store_file(source_path);

    require_equal(
        second_store_result.manifest_id,
        first_store_result.manifest_id,
        "Service deterministic manifest-ID test"
    );

    require_equal(
        second_store_result.chunks_stored,
        static_cast<std::size_t>(0),
        "Service repeated-store new-chunk count test"
    );

    require_equal(
        second_store_result.chunks_reused,
        static_cast<std::size_t>(3),
        "Service repeated-store reused-chunk count test"
    );

    require_true(
        !second_store_result.manifest_stored,
        "Service manifest deduplication test"
    );

    const auto inspect_result =
        service.inspect_file(
            first_store_result.manifest_id
        );

    require_equal(
        inspect_result.manifest_id,
        first_store_result.manifest_id,
        "Service inspection manifest-ID test"
    );

    require_equal(
        inspect_result.original_filename,
        std::string{"source.bin"},
        "Service inspection filename test"
    );

    require_equal(
        inspect_result.file_size,
        static_cast<std::uint64_t>(file_size),
        "Service inspection file-size test"
    );

    require_equal(
        inspect_result.configured_chunk_size,
        chunk_size,
        "Service inspection chunk-size test"
    );

    require_equal(
        inspect_result.chunks.size(),
        static_cast<std::size_t>(3),
        "Service inspection chunk-count test"
    );

    require_equal(
        inspect_result.available_chunks,
        static_cast<std::size_t>(3),
        "Service inspection available-chunk test"
    );

    require_equal(
        inspect_result.missing_chunks,
        static_cast<std::size_t>(0),
        "Service inspection missing-chunk test"
    );

    for (const auto& chunk : inspect_result.chunks)
    {
        require_true(
            chunk.present,
            "Service inspected chunk-presence test"
        );

        require_equal(
            chunk.hash.size(),
            static_cast<std::size_t>(64),
            "Service inspected chunk-hash length test"
        );
    }

    const auto verify_result =
        service.verify_file(
            first_store_result.manifest_id
        );

    require_equal(
        verify_result.manifest_id,
        first_store_result.manifest_id,
        "Service verification manifest-ID test"
    );

    require_equal(
        verify_result.file_size,
        static_cast<std::uint64_t>(file_size),
        "Service verification file-size test"
    );

    require_equal(
        verify_result.chunk_count,
        static_cast<std::size_t>(3),
        "Service verification chunk-count test"
    );

    require_equal(
        verify_result.verified_chunks.size(),
        static_cast<std::size_t>(3),
        "Service verified-chunk list test"
    );

    require_equal(
        verify_result.total_bytes_verified,
        static_cast<std::uint64_t>(file_size),
        "Service verified-byte count test"
    );

    const auto list_result =
        service.list_files();

    require_equal(
        list_result.files.size(),
        static_cast<std::size_t>(1),
        "Service catalog file-count test"
    );

    require_equal(
        list_result.complete_manifests,
        static_cast<std::size_t>(1),
        "Service complete-manifest count test"
    );

    require_equal(
        list_result.incomplete_manifests,
        static_cast<std::size_t>(0),
        "Service incomplete-manifest count test"
    );

    require_equal(
        list_result.files.front().manifest_id,
        first_store_result.manifest_id,
        "Service catalog manifest-ID test"
    );

    require_equal(
        list_result.files.front().missing_chunks,
        static_cast<std::size_t>(0),
        "Service catalog missing-chunk test"
    );

    std::error_code removal_error;

    const bool source_removed =
        std::filesystem::remove(
            source_path,
            removal_error
        );

    if (removal_error)
    {
        throw std::runtime_error(
            "Failed to remove source file before service restoration: "
            + removal_error.message()
        );
    }

    require_true(
        source_removed,
        "Service source-file removal test"
    );

    require_true(
        !std::filesystem::exists(source_path),
        "Service source path must be absent before restoration"
    );

    const auto restore_result =
        service.restore_file(
            first_store_result.manifest_id,
            restored_path
        );

    require_equal(
        restore_result.manifest_id,
        first_store_result.manifest_id,
        "Service restore manifest-ID test"
    );

    require_equal(
        restore_result.original_filename,
        std::string{"source.bin"},
        "Service restore filename test"
    );

    require_equal(
        restore_result.output_path,
        restored_path,
        "Service restore output-path test"
    );

    require_equal(
        restore_result.bytes_written,
        static_cast<std::uint64_t>(file_size),
        "Service restore byte-count test"
    );

    require_equal(
        restore_result.chunks_loaded,
        static_cast<std::size_t>(3),
        "Service restore chunk-count test"
    );

    const auto restored_data =
        read_binary_file(restored_path);

    require_equal(
        restored_data,
        original_data,
        "Service byte-perfect restoration test"
    );

    require_throws(
        [&service, &first_store_result, &restored_path]()
        {
            (void)service.restore_file(
                first_store_result.manifest_id,
                restored_path
            );
        },
        "Service overwrite-protection test"
    );
}

}

int main()
{
    try
    {
        test_service_constructor_validation();

        std::cout
            << "[PASS] Service constructor validation\n";

        test_service_storage_workflow();

        std::cout
            << "[PASS] Service storage workflow\n";

        std::cout
            << "All NexusFS service tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "[FAIL] "
            << error.what()
            << '\n';

        return 1;
    }
}