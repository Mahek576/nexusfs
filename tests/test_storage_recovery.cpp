#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/storage_recovery.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

namespace
{

using nexusfs::observability::
    MetricsRegistry;

using nexusfs::storage::
    StorageRecoveryReport;

using nexusfs::storage::
    recover_storage_root;

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        static std::atomic<std::uint64_t>
            sequence{
                0
            };

        path_ =
            std::filesystem::temp_directory_path()
            / (
                "nexusfs-storage-recovery-tests-"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count()
                )
                + "-"
                + std::to_string(
                    sequence.fetch_add(
                        1,
                        std::memory_order_relaxed
                    )
                )
            );

        std::filesystem::create_directories(
            path_
        );
    }

    ~TemporaryDirectory()
    {
        std::error_code cleanup_error;

        std::filesystem::remove_all(
            path_,
            cleanup_error
        );
    }

    [[nodiscard]]
    const std::filesystem::path&
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

template <
    typename Actual,
    typename Expected
>
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

template <typename Operation>
void require_exception(
    Operation&& operation,
    const std::string& test_name
)
{
    bool exception_thrown =
        false;

    try
    {
        operation();
    }
    catch (const std::exception&)
    {
        exception_thrown =
            true;
    }

    require_true(
        exception_thrown,
        test_name
    );
}

void write_file(
    const std::filesystem::path& path,
    const std::string& contents
)
{
    std::filesystem::create_directories(
        path.parent_path()
    );

    std::ofstream output{
        path,
        std::ios::binary
            | std::ios::trunc
    };

    if (!output.is_open())
    {
        throw std::runtime_error(
            "Failed to create recovery test file."
        );
    }

    output.write(
        contents.data(),
        static_cast<std::streamsize>(
            contents.size()
        )
    );

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write recovery test file."
        );
    }
}

void test_missing_storage_root()
{
    TemporaryDirectory directory;

    const StorageRecoveryReport report =
        recover_storage_root(
            directory.path()
            / "missing"
        );

    require_equal(
        report.entries_scanned,
        static_cast<std::uint64_t>(0),
        "Missing-root scan test"
    );

    require_equal(
        report.temporary_files_removed,
        static_cast<std::uint64_t>(0),
        "Missing-root removal test"
    );
}

void test_temporary_file_cleanup()
{
    TemporaryDirectory directory;

    const std::filesystem::path root =
        directory.path()
        / "storage";

    const std::filesystem::path chunk_temp =
        root
        / "chunks"
        / "aa"
        / "chunk.tmp.100.200.1";

    const std::filesystem::path manifest_temp =
        root
        / "manifests"
        / "bb"
        / "manifest.tmp.101.201.2";

    const std::filesystem::path unrelated_file =
        root
        / "chunks"
        / "aa"
        / "notes.tmp.backup";

    const std::filesystem::path malformed_temp =
        root
        / "manifests"
        / "bb"
        / "manifest.tmp.1.invalid.3";

    const std::filesystem::path candidate_directory =
        root
        / "chunks"
        / "cc"
        / "directory.tmp.102.202.3";

    write_file(
        chunk_temp,
        "abandoned-chunk"
    );

    write_file(
        manifest_temp,
        "abandoned-manifest"
    );

    write_file(
        unrelated_file,
        "preserve"
    );

    write_file(
        malformed_temp,
        "preserve"
    );

    std::filesystem::create_directories(
        candidate_directory
    );

    const StorageRecoveryReport report =
        recover_storage_root(
            root
        );

    require_equal(
        report.temporary_entries_found,
        static_cast<std::uint64_t>(3),
        "Recovery candidate-count test"
    );

    require_equal(
        report.temporary_files_removed,
        static_cast<std::uint64_t>(2),
        "Recovery removed-file test"
    );

    require_equal(
        report.non_regular_entries_preserved,
        static_cast<std::uint64_t>(1),
        "Recovery preserved-entry test"
    );

    require_true(
        !std::filesystem::exists(
            chunk_temp
        ),
        "Abandoned chunk cleanup test"
    );

    require_true(
        !std::filesystem::exists(
            manifest_temp
        ),
        "Abandoned manifest cleanup test"
    );

    require_true(
        std::filesystem::exists(
            unrelated_file
        ),
        "Unrelated temporary-name preservation test"
    );

    require_true(
        std::filesystem::exists(
            malformed_temp
        ),
        "Malformed temporary-name preservation test"
    );

    require_true(
        std::filesystem::is_directory(
            candidate_directory
        ),
        "Non-regular recovery candidate test"
    );

    const StorageRecoveryReport second_report =
        recover_storage_root(
            root
        );

    require_equal(
        second_report.temporary_files_removed,
        static_cast<std::uint64_t>(0),
        "Recovery idempotence test"
    );
}

void test_invalid_storage_root()
{
    TemporaryDirectory directory;

    const std::filesystem::path root_file =
        directory.path()
        / "not-a-directory";

    write_file(
        root_file,
        "content"
    );

    require_exception(
        [&root_file]()
        {
            (void)recover_storage_root(
                root_file
            );
        },
        "Invalid recovery-root rejection test"
    );
}

void test_recovery_metrics_and_endpoint()
{
    TemporaryDirectory directory;

    const std::filesystem::path root =
        directory.path()
        / "storage";

    const auto metrics =
        std::make_shared<
            MetricsRegistry
        >();

    metrics->record_storage_recovery(
        17,
        4,
        3,
        1
    );

    const auto snapshot =
        metrics->snapshot();

    require_equal(
        snapshot.recovery_runs_total,
        static_cast<std::uint64_t>(1),
        "Recovery run metric test"
    );

    require_equal(
        snapshot.recovery_entries_scanned,
        static_cast<std::uint64_t>(17),
        "Recovery scanned-entry metric test"
    );

    require_equal(
        snapshot
            .recovery_temporary_files_removed,
        static_cast<std::uint64_t>(3),
        "Recovery removed-file metric test"
    );

    const auto service =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            root,
            1024
        );

    const nexusfs::http::HttpRouter router{
        service,
        metrics
    };

    nexusfs::http::HttpRouter::Request request{
        boost::beast::http::verb::get,
        "/api/v1/metrics",
        11
    };

    request.keep_alive(
        false
    );

    const auto response =
        router.route(
            request
        );

    require_equal(
        response.result(),
        boost::beast::http::status::ok,
        "Recovery metrics endpoint status test"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    require_equal(
        payload
            .at("recovery")
            .at("runs")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1),
        "Recovery endpoint run-count test"
    );

    require_equal(
        payload
            .at("recovery")
            .at("temporary_files_removed")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(3),
        "Recovery endpoint removal-count test"
    );
}

}

int main()
{
    try
    {
        test_missing_storage_root();

        std::cout
            << "[PASS] Storage recovery missing root\n";

        test_temporary_file_cleanup();

        std::cout
            << "[PASS] Storage recovery cleanup\n";

        test_invalid_storage_root();

        std::cout
            << "[PASS] Storage recovery validation\n";

        test_recovery_metrics_and_endpoint();

        std::cout
            << "[PASS] Storage recovery observability\n";

        std::cout
            << "All NexusFS storage recovery tests passed.\n";

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
