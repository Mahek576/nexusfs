#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/chunker.hpp"
#include "nexusfs/storage/file_manifest.hpp"
#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/manifest_store.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{

constexpr std::chrono::seconds
    gate_timeout{
        10
    };

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
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

        path_ =
            std::filesystem::temp_directory_path()
            / (
                "nexusfs-concurrency-tests-"
                + std::to_string(timestamp)
                + "-"
                + std::to_string(current_sequence)
            );

        std::error_code directory_error;

        std::filesystem::create_directories(
            path_,
            directory_error
        );

        if (directory_error)
        {
            throw std::runtime_error(
                "Failed to create concurrency-test directory: "
                + directory_error.message()
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
        std::error_code cleanup_error;

        std::filesystem::remove_all(
            path_,
            cleanup_error
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

class StartGate final
{
public:
    explicit StartGate(
        std::size_t participant_count
    )
        : participant_count_{
              participant_count
          }
    {
        if (participant_count_ == 0)
        {
            throw std::invalid_argument(
                "Start gate requires at least one participant."
            );
        }
    }

    void arrive_and_wait()
    {
        std::unique_lock lock{
            mutex_
        };

        ++ready_count_;

        condition_.notify_all();

        condition_.wait(
            lock,
            [this]()
            {
                return released_;
            }
        );
    }

    void release_when_ready()
    {
        std::unique_lock lock{
            mutex_
        };

        const bool all_ready =
            condition_.wait_for(
                lock,
                gate_timeout,
                [this]()
                {
                    return (
                        ready_count_
                        == participant_count_
                    );
                }
            );

        /*
         * Always release waiting workers, even when the timeout
         * indicates a test setup failure.
         */
        released_ =
            true;

        lock.unlock();

        condition_.notify_all();

        if (!all_ready)
        {
            throw std::runtime_error(
                "Concurrent test workers did not "
                "reach the start gate in time."
            );
        }
    }

private:
    std::size_t participant_count_;
    std::size_t ready_count_{0};
    bool released_{false};

    std::mutex mutex_;
    std::condition_variable condition_;
};

class ExceptionCollector final
{
public:
    void capture_current_exception()
    {
        const std::lock_guard lock{
            mutex_
        };

        exceptions_.push_back(
            std::current_exception()
        );
    }

    void rethrow_first() const
    {
        std::exception_ptr exception;

        {
            const std::lock_guard lock{
                mutex_
            };

            if (!exceptions_.empty())
            {
                exception =
                    exceptions_.front();
            }
        }

        if (exception)
        {
            std::rethrow_exception(
                exception
            );
        }
    }

private:
    mutable std::mutex mutex_;

    std::vector<std::exception_ptr>
        exceptions_;
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

void join_threads(
    std::vector<std::thread>& threads
)
{
    for (
        std::thread& thread :
        threads
    )
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
}

std::vector<std::uint8_t> create_test_data(
    std::size_t size,
    std::uint64_t seed
)
{
    std::vector<std::uint8_t> data(
        size
    );

    for (
        std::size_t index = 0;
        index < data.size();
        ++index
    )
    {
        const std::uint64_t block_number =
            static_cast<std::uint64_t>(
                index / 4096
            );

        const std::uint64_t value =
            (
                static_cast<std::uint64_t>(
                    index
                ) * 37ULL
                + block_number * 53ULL
                + seed * 97ULL
                + (
                    static_cast<std::uint64_t>(
                        index >> 8U
                    )
                    * 11ULL
                )
            )
            % 256ULL;

        data[index] =
            static_cast<std::uint8_t>(
                value
            );
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
                "Failed to create test-file directory: "
                + directory_error.message()
            );
        }
    }

    if (
        data.size() >
        static_cast<std::size_t>(
            std::numeric_limits<
                std::streamsize
            >::max()
        )
    )
    {
        throw std::runtime_error(
            "Test data is too large for stream output."
        );
    }

    std::ofstream output{
        path,
        std::ios::binary
            | std::ios::trunc
    };

    if (!output.is_open())
    {
        throw std::runtime_error(
            "Failed to create test file: "
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
            "Failed while writing test file: "
            + path.string()
        );
    }
}

std::vector<std::uint8_t> read_binary_file(
    const std::filesystem::path& path
)
{
    std::error_code size_error;

    const std::uintmax_t file_size =
        std::filesystem::file_size(
            path,
            size_error
        );

    if (size_error)
    {
        throw std::runtime_error(
            "Failed to determine test-file size: "
            + size_error.message()
        );
    }

    if (
        file_size >
        static_cast<std::uintmax_t>(
            std::numeric_limits<
                std::size_t
            >::max()
        )
    )
    {
        throw std::runtime_error(
            "Test file is too large to load."
        );
    }

    if (
        file_size >
        static_cast<std::uintmax_t>(
            std::numeric_limits<
                std::streamsize
            >::max()
        )
    )
    {
        throw std::runtime_error(
            "Test file is too large for stream input."
        );
    }

    std::ifstream input{
        path,
        std::ios::binary
    };

    if (!input.is_open())
    {
        throw std::runtime_error(
            "Failed to open test file: "
            + path.string()
        );
    }

    std::vector<std::uint8_t> data(
        static_cast<std::size_t>(
            file_size
        )
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
                "Failed while reading test file: "
                + path.string()
            );
        }
    }

    return data;
}

void require_no_temporary_files(
    const std::filesystem::path& root,
    const std::string& test_name
)
{
    if (
        !std::filesystem::exists(
            root
        )
    )
    {
        return;
    }

    std::error_code iterator_error;

    std::filesystem::recursive_directory_iterator
        iterator{
            root,
            iterator_error
        };

    if (iterator_error)
    {
        throw std::runtime_error(
            test_name
            + " failed while opening the storage tree: "
            + iterator_error.message()
        );
    }

    const std::filesystem::
        recursive_directory_iterator end;

    while (
        iterator != end
    )
    {
        const std::filesystem::path path =
            iterator->path();

        if (
            path.filename()
                .string()
                .find(".tmp.")
            != std::string::npos
        )
        {
            throw std::runtime_error(
                test_name
                + " left a temporary file behind: "
                + path.string()
            );
        }

        iterator.increment(
            iterator_error
        );

        if (iterator_error)
        {
            throw std::runtime_error(
                test_name
                + " failed while scanning the storage tree: "
                + iterator_error.message()
            );
        }
    }
}

void test_concurrent_low_level_publication()
{
    TemporaryDirectory directory;

    const std::filesystem::path storage_root =
        directory.path()
        / "low-level-storage";

    const std::vector<std::uint8_t> data =
        create_test_data(
            65537,
            11
        );

    const std::string chunk_hash =
        nexusfs::storage::Sha256Hasher::hash(
            data
        );

    const nexusfs::storage::FileChunk chunk{
        0,
        data,
        chunk_hash
    };

    constexpr std::size_t thread_count =
        12;

    StartGate chunk_gate{
        thread_count
    };

    ExceptionCollector chunk_exceptions;

    std::vector<
        nexusfs::storage::StoreResult
    > chunk_results(
        thread_count,
        nexusfs::storage::
            StoreResult::already_exists
    );

    std::vector<std::thread>
        chunk_threads;

    chunk_threads.reserve(
        thread_count
    );

    for (
        std::size_t index = 0;
        index < thread_count;
        ++index
    )
    {
        chunk_threads.emplace_back(
            [
                &storage_root,
                &chunk,
                &chunk_gate,
                &chunk_exceptions,
                &chunk_results,
                index
            ]()
            {
                try
                {
                    nexusfs::storage::ChunkStore
                        store{
                            storage_root
                        };

                    chunk_gate.arrive_and_wait();

                    chunk_results[index] =
                        store.store(
                            chunk
                        );
                }
                catch (...)
                {
                    chunk_exceptions
                        .capture_current_exception();
                }
            }
        );
    }

    chunk_gate.release_when_ready();

    join_threads(
        chunk_threads
    );

    chunk_exceptions.rethrow_first();

    std::size_t chunk_result_count = 0;

    for (
        const nexusfs::storage::StoreResult result :
        chunk_results
    )
    {
        if (
            result ==
                nexusfs::storage::
                    StoreResult::stored
            || result ==
                nexusfs::storage::
                    StoreResult::already_exists
        )
        {
            ++chunk_result_count;
        }
    }

    require_equal(
        chunk_result_count,
        thread_count,
        "Concurrent low-level chunk result test"
    );

    const nexusfs::storage::ChunkStore
        final_chunk_store{
            storage_root
        };

    require_equal(
        final_chunk_store.load(
            chunk_hash
        ),
        data,
        "Concurrent low-level chunk integrity test"
    );

    const nexusfs::storage::FileManifest
        manifest =
            nexusfs::storage::FileManifest::restore(
                "concurrent.bin",
                static_cast<std::uint64_t>(
                    data.size()
                ),
                data.size(),
                std::vector<std::string>{
                    chunk_hash
                }
            );

    const std::vector<std::uint8_t>
        encoded_manifest =
            nexusfs::storage::
                FileManifestCodec::encode(
                    manifest
                );

    const std::string manifest_id =
        nexusfs::storage::Sha256Hasher::hash(
            encoded_manifest
        );

    StartGate manifest_gate{
        thread_count
    };

    ExceptionCollector manifest_exceptions;

    std::vector<
        nexusfs::storage::
            ManifestStoreResult
    > manifest_results(
        thread_count,
        nexusfs::storage::
            ManifestStoreResult::already_exists
    );

    std::vector<std::thread>
        manifest_threads;

    manifest_threads.reserve(
        thread_count
    );

    for (
        std::size_t index = 0;
        index < thread_count;
        ++index
    )
    {
        manifest_threads.emplace_back(
            [
                &storage_root,
                &manifest_id,
                &encoded_manifest,
                &manifest_gate,
                &manifest_exceptions,
                &manifest_results,
                index
            ]()
            {
                try
                {
                    nexusfs::storage::ManifestStore
                        store{
                            storage_root
                        };

                    manifest_gate.arrive_and_wait();

                    manifest_results[index] =
                        store.store(
                            manifest_id,
                            encoded_manifest
                        );
                }
                catch (...)
                {
                    manifest_exceptions
                        .capture_current_exception();
                }
            }
        );
    }

    manifest_gate.release_when_ready();

    join_threads(
        manifest_threads
    );

    manifest_exceptions.rethrow_first();

    std::size_t manifest_result_count = 0;

    for (
        const nexusfs::storage::
            ManifestStoreResult result :
        manifest_results
    )
    {
        if (
            result ==
                nexusfs::storage::
                    ManifestStoreResult::stored
            || result ==
                nexusfs::storage::
                    ManifestStoreResult::
                        already_exists
        )
        {
            ++manifest_result_count;
        }
    }

    require_equal(
        manifest_result_count,
        thread_count,
        "Concurrent low-level manifest result test"
    );

    const nexusfs::storage::ManifestStore
        final_manifest_store{
            storage_root
        };

    require_equal(
        final_manifest_store.load(
            manifest_id
        ),
        encoded_manifest,
        "Concurrent low-level manifest integrity test"
    );

    require_no_temporary_files(
        storage_root,
        "Concurrent low-level publication cleanup test"
    );
}

void test_concurrent_duplicate_service_store()
{
    TemporaryDirectory directory;

    const std::filesystem::path storage_root =
        directory.path()
        / "duplicate-storage";

    const std::filesystem::path source_path =
        directory.path()
        / "sources"
        / "duplicate.bin";

    constexpr std::size_t chunk_size =
        4096;

    const std::vector<std::uint8_t> source_data =
        create_test_data(
            131071,
            23
        );

    write_binary_file(
        source_path,
        source_data
    );

    constexpr std::size_t thread_count =
        8;

    std::vector<
        std::shared_ptr<
            nexusfs::app::NexusFsService
        >
    > services;

    services.reserve(
        thread_count
    );

    for (
        std::size_t index = 0;
        index < thread_count;
        ++index
    )
    {
        const std::filesystem::path service_root =
            (
                index % 2 == 0
            )
            ? storage_root
            : storage_root / ".";

        services.push_back(
            std::make_shared<
                nexusfs::app::NexusFsService
            >(
                service_root,
                chunk_size
            )
        );
    }

    StartGate gate{
        thread_count
    };

    ExceptionCollector exceptions;

    std::vector<
        nexusfs::app::StoreFileResult
    > results(
        thread_count
    );

    std::vector<std::thread> threads;

    threads.reserve(
        thread_count
    );

    for (
        std::size_t index = 0;
        index < thread_count;
        ++index
    )
    {
        threads.emplace_back(
            [
                &services,
                &source_path,
                &gate,
                &exceptions,
                &results,
                index
            ]()
            {
                try
                {
                    gate.arrive_and_wait();

                    results[index] =
                        services[index]->
                            store_file(
                                source_path
                            );
                }
                catch (...)
                {
                    exceptions
                        .capture_current_exception();
                }
            }
        );
    }

    gate.release_when_ready();

    join_threads(
        threads
    );

    exceptions.rethrow_first();

    const std::string manifest_id =
        results.front().manifest_id;

    require_true(
        !manifest_id.empty(),
        "Concurrent duplicate manifest-ID test"
    );

    std::size_t new_manifest_count = 0;
    std::size_t writers_with_new_chunks = 0;

    for (
        const nexusfs::app::StoreFileResult&
            result :
        results
    )
    {
        require_equal(
            result.manifest_id,
            manifest_id,
            "Concurrent duplicate manifest consistency test"
        );

        require_equal(
            result.file_size,
            static_cast<std::uint64_t>(
                source_data.size()
            ),
            "Concurrent duplicate file-size test"
        );

        if (result.manifest_stored)
        {
            ++new_manifest_count;
        }

        if (result.chunks_stored != 0)
        {
            ++writers_with_new_chunks;
        }
    }

    require_equal(
        new_manifest_count,
        static_cast<std::size_t>(1),
        "Concurrent duplicate manifest publication test"
    );

    require_equal(
        writers_with_new_chunks,
        static_cast<std::size_t>(1),
        "Concurrent duplicate chunk publication test"
    );

    const nexusfs::app::ListFilesResult catalog =
        services.front()->
            list_files();

    require_equal(
        catalog.files.size(),
        static_cast<std::size_t>(1),
        "Concurrent duplicate catalog-count test"
    );

    require_equal(
        catalog.complete_manifests,
        static_cast<std::size_t>(1),
        "Concurrent duplicate complete-catalog test"
    );

    require_equal(
        catalog.incomplete_manifests,
        static_cast<std::size_t>(0),
        "Concurrent duplicate incomplete-catalog test"
    );

    const nexusfs::app::VerifyFileResult
        verification =
            services.front()->
                verify_file(
                    manifest_id
                );

    require_equal(
        verification.total_bytes_verified,
        static_cast<std::uint64_t>(
            source_data.size()
        ),
        "Concurrent duplicate verification test"
    );

    require_no_temporary_files(
        storage_root,
        "Concurrent duplicate cleanup test"
    );
}

void test_concurrent_catalog_readers_and_writers()
{
    TemporaryDirectory directory;

    const std::filesystem::path storage_root =
        directory.path()
        / "catalog-storage";

    constexpr std::size_t chunk_size =
        2048;

    constexpr std::size_t writer_count =
        5;

    constexpr std::size_t reader_count =
        3;

    std::vector<std::filesystem::path>
        source_paths;

    std::vector<
        std::vector<std::uint8_t>
    > source_data;

    source_paths.reserve(
        writer_count
    );

    source_data.reserve(
        writer_count
    );

    for (
        std::size_t index = 0;
        index < writer_count;
        ++index
    )
    {
        const std::filesystem::path source_path =
            directory.path()
            / "catalog-sources"
            / (
                "file-"
                + std::to_string(index)
                + ".bin"
            );

        std::vector<std::uint8_t> data =
            create_test_data(
                70000
                    + index * 1731,
                100
                    + static_cast<std::uint64_t>(
                        index
                    )
            );

        write_binary_file(
            source_path,
            data
        );

        source_paths.push_back(
            source_path
        );

        source_data.push_back(
            std::move(data)
        );
    }

    std::vector<
        std::shared_ptr<
            nexusfs::app::NexusFsService
        >
    > writer_services;

    writer_services.reserve(
        writer_count
    );

    for (
        std::size_t index = 0;
        index < writer_count;
        ++index
    )
    {
        writer_services.push_back(
            std::make_shared<
                nexusfs::app::NexusFsService
            >(
                storage_root,
                chunk_size
            )
        );
    }

    std::vector<
        std::shared_ptr<
            nexusfs::app::NexusFsService
        >
    > reader_services;

    reader_services.reserve(
        reader_count
    );

    for (
        std::size_t index = 0;
        index < reader_count;
        ++index
    )
    {
        reader_services.push_back(
            std::make_shared<
                nexusfs::app::NexusFsService
            >(
                storage_root / ".",
                chunk_size
            )
        );
    }

    StartGate gate{
        writer_count
        + reader_count
    };

    ExceptionCollector exceptions;

    std::atomic<bool> writers_done{
        false
    };

    std::vector<
        nexusfs::app::StoreFileResult
    > writer_results(
        writer_count
    );

    std::vector<std::thread>
        writer_threads;

    std::vector<std::thread>
        reader_threads;

    writer_threads.reserve(
        writer_count
    );

    reader_threads.reserve(
        reader_count
    );

    for (
        std::size_t index = 0;
        index < writer_count;
        ++index
    )
    {
        writer_threads.emplace_back(
            [
                &writer_services,
                &source_paths,
                &writer_results,
                &gate,
                &exceptions,
                index
            ]()
            {
                try
                {
                    gate.arrive_and_wait();

                    writer_results[index] =
                        writer_services[index]->
                            store_file(
                                source_paths[index]
                            );
                }
                catch (...)
                {
                    exceptions
                        .capture_current_exception();
                }
            }
        );
    }

    for (
        std::size_t index = 0;
        index < reader_count;
        ++index
    )
    {
        reader_threads.emplace_back(
            [
                &reader_services,
                &writers_done,
                &gate,
                &exceptions,
                index
            ]()
            {
                try
                {
                    gate.arrive_and_wait();

                    std::size_t iteration = 0;

                    do
                    {
                        const nexusfs::app::
                            ListFilesResult catalog =
                                reader_services[index]->
                                    list_files();

                        require_equal(
                            catalog.complete_manifests
                                + catalog.incomplete_manifests,
                            catalog.files.size(),
                            "Concurrent catalog summary test"
                        );

                        require_equal(
                            catalog.incomplete_manifests,
                            static_cast<std::size_t>(0),
                            "Concurrent catalog completeness test"
                        );

                        for (
                            const nexusfs::app::
                                StoredFileSummary& file :
                            catalog.files
                        )
                        {
                            require_equal(
                                file.missing_chunks,
                                static_cast<std::size_t>(0),
                                "Concurrent catalog missing-chunk test"
                            );

                            const nexusfs::app::
                                InspectFileResult inspection =
                                    reader_services[index]->
                                        inspect_file(
                                            file.manifest_id
                                        );

                            require_equal(
                                inspection.missing_chunks,
                                static_cast<std::size_t>(0),
                                "Concurrent inspection consistency test"
                            );
                        }

                        ++iteration;

                        std::this_thread::sleep_for(
                            std::chrono::milliseconds{
                                1
                            }
                        );
                    }
                    while (
                        !writers_done.load(
                            std::memory_order_acquire
                        )
                        || iteration < 25
                    );
                }
                catch (...)
                {
                    exceptions
                        .capture_current_exception();
                }
            }
        );
    }

    gate.release_when_ready();

    join_threads(
        writer_threads
    );

    writers_done.store(
        true,
        std::memory_order_release
    );

    join_threads(
        reader_threads
    );

    exceptions.rethrow_first();

    const nexusfs::app::ListFilesResult final_catalog =
        reader_services.front()->
            list_files();

    require_equal(
        final_catalog.files.size(),
        writer_count,
        "Concurrent final catalog-count test"
    );

    require_equal(
        final_catalog.complete_manifests,
        writer_count,
        "Concurrent final complete-count test"
    );

    require_equal(
        final_catalog.incomplete_manifests,
        static_cast<std::size_t>(0),
        "Concurrent final incomplete-count test"
    );

    std::set<std::string> manifest_ids;

    for (
        std::size_t index = 0;
        index < writer_results.size();
        ++index
    )
    {
        manifest_ids.insert(
            writer_results[index]
                .manifest_id
        );

        const nexusfs::app::VerifyFileResult
            verification =
                reader_services.front()->
                    verify_file(
                        writer_results[index]
                            .manifest_id
                    );

        require_equal(
            verification.total_bytes_verified,
            static_cast<std::uint64_t>(
                source_data[index].size()
            ),
            "Concurrent final verification test"
        );
    }

    require_equal(
        manifest_ids.size(),
        writer_count,
        "Concurrent distinct-manifest test"
    );

    require_no_temporary_files(
        storage_root,
        "Concurrent reader-writer cleanup test"
    );
}

void test_same_output_restore_serialization()
{
    TemporaryDirectory directory;

    const std::filesystem::path storage_root =
        directory.path()
        / "same-output-storage";

    const std::filesystem::path source_path =
        directory.path()
        / "same-output-source.bin";

    const std::filesystem::path output_path =
        directory.path()
        / "restored"
        / "same-output.bin";

    const std::vector<std::uint8_t> source_data =
        create_test_data(
            220001,
            307
        );

    write_binary_file(
        source_path,
        source_data
    );

    nexusfs::app::NexusFsService
        initial_service{
            storage_root,
            4096
        };

    const nexusfs::app::StoreFileResult stored =
        initial_service.store_file(
            source_path
        );

    auto first_service =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            storage_root,
            4096
        );

    auto second_service =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            storage_root / ".",
            4096
        );

    StartGate gate{
        2
    };

    std::atomic<std::size_t> success_count{
        0
    };

    std::atomic<std::size_t> failure_count{
        0
    };

    std::mutex messages_mutex;

    std::vector<std::string>
        failure_messages;

    const auto restore_operation =
        [
            &stored,
            &output_path,
            &gate,
            &success_count,
            &failure_count,
            &messages_mutex,
            &failure_messages
        ](
            const std::shared_ptr<
                nexusfs::app::NexusFsService
            >& service
        )
        {
            gate.arrive_and_wait();

            try
            {
                (void)service->restore_file(
                    stored.manifest_id,
                    output_path
                );

                success_count.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            }
            catch (const std::exception& error)
            {
                failure_count.fetch_add(
                    1,
                    std::memory_order_relaxed
                );

                const std::lock_guard lock{
                    messages_mutex
                };

                failure_messages.push_back(
                    error.what()
                );
            }
            catch (...)
            {
                failure_count.fetch_add(
                    1,
                    std::memory_order_relaxed
                );

                const std::lock_guard lock{
                    messages_mutex
                };

                failure_messages.push_back(
                    "unknown exception"
                );
            }
        };

    std::vector<std::thread> threads;

    threads.emplace_back(
        restore_operation,
        first_service
    );

    threads.emplace_back(
        restore_operation,
        second_service
    );

    gate.release_when_ready();

    join_threads(
        threads
    );

    require_equal(
        success_count.load(
            std::memory_order_relaxed
        ),
        static_cast<std::size_t>(1),
        "Same-output restoration success-count test"
    );

    require_equal(
        failure_count.load(
            std::memory_order_relaxed
        ),
        static_cast<std::size_t>(1),
        "Same-output restoration failure-count test"
    );

    require_equal(
        read_binary_file(
            output_path
        ),
        source_data,
        "Same-output restored-byte test"
    );

    require_equal(
        failure_messages.size(),
        static_cast<std::size_t>(1),
        "Same-output failure-message count test"
    );

    require_true(
        failure_messages.front().find(
            "already exists"
        )
        != std::string::npos,
        "Same-output overwrite-rejection test"
    );

    require_no_temporary_files(
        directory.path(),
        "Same-output restoration cleanup test"
    );
}

void test_distinct_output_restorations()
{
    TemporaryDirectory directory;

    const std::filesystem::path storage_root =
        directory.path()
        / "distinct-output-storage";

    const std::filesystem::path source_path =
        directory.path()
        / "distinct-output-source.bin";

    const std::vector<std::uint8_t> source_data =
        create_test_data(
            250003,
            401
        );

    write_binary_file(
        source_path,
        source_data
    );

    nexusfs::app::NexusFsService
        initial_service{
            storage_root,
            4096
        };

    const nexusfs::app::StoreFileResult stored =
        initial_service.store_file(
            source_path
        );

    constexpr std::size_t thread_count =
        6;

    std::vector<
        std::shared_ptr<
            nexusfs::app::NexusFsService
        >
    > services;

    std::vector<std::filesystem::path>
        output_paths;

    services.reserve(
        thread_count
    );

    output_paths.reserve(
        thread_count
    );

    for (
        std::size_t index = 0;
        index < thread_count;
        ++index
    )
    {
        services.push_back(
            std::make_shared<
                nexusfs::app::NexusFsService
            >(
                storage_root,
                4096
            )
        );

        output_paths.push_back(
            directory.path()
            / "distinct-restores"
            / (
                "copy-"
                + std::to_string(index)
                + ".bin"
            )
        );
    }

    StartGate gate{
        thread_count
    };

    ExceptionCollector exceptions;

    std::vector<
        nexusfs::app::RestoreFileResult
    > results(
        thread_count
    );

    std::vector<std::thread> threads;

    threads.reserve(
        thread_count
    );

    for (
        std::size_t index = 0;
        index < thread_count;
        ++index
    )
    {
        threads.emplace_back(
            [
                &services,
                &output_paths,
                &stored,
                &results,
                &gate,
                &exceptions,
                index
            ]()
            {
                try
                {
                    gate.arrive_and_wait();

                    results[index] =
                        services[index]->
                            restore_file(
                                stored.manifest_id,
                                output_paths[index]
                            );
                }
                catch (...)
                {
                    exceptions
                        .capture_current_exception();
                }
            }
        );
    }

    gate.release_when_ready();

    join_threads(
        threads
    );

    exceptions.rethrow_first();

    for (
        std::size_t index = 0;
        index < thread_count;
        ++index
    )
    {
        require_equal(
            results[index].bytes_written,
            static_cast<std::uint64_t>(
                source_data.size()
            ),
            "Distinct-output byte-count test"
        );

        require_equal(
            read_binary_file(
                output_paths[index]
            ),
            source_data,
            "Distinct-output restored-byte test"
        );
    }

    require_no_temporary_files(
        directory.path(),
        "Distinct-output restoration cleanup test"
    );
}

}

int main()
{
    try
    {
        test_concurrent_low_level_publication();

        std::cout
            << "[PASS] Concurrent low-level publication\n";

        test_concurrent_duplicate_service_store();

        std::cout
            << "[PASS] Concurrent duplicate service storage\n";

        test_concurrent_catalog_readers_and_writers();

        std::cout
            << "[PASS] Concurrent catalog readers and writers\n";

        test_same_output_restore_serialization();

        std::cout
            << "[PASS] Same-output restoration serialization\n";

        test_distinct_output_restorations();

        std::cout
            << "[PASS] Distinct-output concurrent restoration\n";

        std::cout
            << "All NexusFS service concurrency tests passed.\n";

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