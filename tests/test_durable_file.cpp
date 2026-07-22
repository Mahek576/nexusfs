#include "nexusfs/storage/durable_file.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

using nexusfs::storage::
    DurablePublishResult;

using nexusfs::storage::
    flush_file_to_disk;

using nexusfs::storage::
    publish_file_durably;

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
                "nexusfs-durable-file-tests-"
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

        std::error_code directory_error;

        std::filesystem::create_directories(
            path_,
            directory_error
        );

        if (directory_error)
        {
            throw std::runtime_error(
                "Failed to create durable-file test directory: "
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
    const std::string& data
)
{
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

    output.write(
        data.data(),
        static_cast<std::streamsize>(
            data.size()
        )
    );

    output.close();

    if (!output)
    {
        throw std::runtime_error(
            "Failed to write test file: "
            + path.string()
        );
    }
}

std::string read_file(
    const std::filesystem::path& path
)
{
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

    return std::string{
        std::istreambuf_iterator<char>{
            input
        },
        std::istreambuf_iterator<char>{}
    };
}

void test_file_flush()
{
    TemporaryDirectory directory;

    const std::filesystem::path path =
        directory.path()
        / "flush.bin";

    const std::string data{
        "durable-file-content"
    };

    write_file(
        path,
        data
    );

    flush_file_to_disk(
        path
    );

    require_equal(
        read_file(
            path
        ),
        data,
        "Durable file-flush content test"
    );
}

void test_successful_publication()
{
    TemporaryDirectory directory;

    const std::filesystem::path temporary_path =
        directory.path()
        / "object.tmp";

    const std::filesystem::path destination_path =
        directory.path()
        / "object.data";

    const std::string data{
        "durably-published-content"
    };

    write_file(
        temporary_path,
        data
    );

    const DurablePublishResult result =
        publish_file_durably(
            temporary_path,
            destination_path
        );

    require_equal(
        result,
        DurablePublishResult::published,
        "Durable publication result test"
    );

    require_true(
        !std::filesystem::exists(
            temporary_path
        ),
        "Durable temporary-name removal test"
    );

    require_equal(
        read_file(
            destination_path
        ),
        data,
        "Durable destination-content test"
    );
}

void test_existing_destination_protection()
{
    TemporaryDirectory directory;

    const std::filesystem::path temporary_path =
        directory.path()
        / "candidate.tmp";

    const std::filesystem::path destination_path =
        directory.path()
        / "existing.data";

    write_file(
        temporary_path,
        "new-content"
    );

    write_file(
        destination_path,
        "existing-content"
    );

    const DurablePublishResult result =
        publish_file_durably(
            temporary_path,
            destination_path
        );

    require_equal(
        result,
        DurablePublishResult::
            destination_exists,
        "Existing destination result test"
    );

    require_equal(
        read_file(
            destination_path
        ),
        std::string{
            "existing-content"
        },
        "Existing destination preservation test"
    );

    require_true(
        std::filesystem::exists(
            temporary_path
        ),
        "Rejected temporary-file retention test"
    );
}

void test_path_validation()
{
    TemporaryDirectory directory;

    const std::filesystem::path first_directory =
        directory.path()
        / "first";

    const std::filesystem::path second_directory =
        directory.path()
        / "second";

    std::filesystem::create_directories(
        first_directory
    );

    std::filesystem::create_directories(
        second_directory
    );

    const std::filesystem::path temporary_path =
        first_directory
        / "temporary.bin";

    write_file(
        temporary_path,
        "content"
    );

    require_exception(
        [&temporary_path]()
        {
            (void)publish_file_durably(
                temporary_path,
                temporary_path
            );
        },
        "Same publication-path rejection test"
    );

    require_exception(
        [
            &temporary_path,
            &second_directory
        ]()
        {
            (void)publish_file_durably(
                temporary_path,
                second_directory
                    / "destination.bin"
            );
        },
        "Cross-directory publication rejection test"
    );

    require_exception(
        [&directory]()
        {
            flush_file_to_disk(
                directory.path()
                / "missing.bin"
            );
        },
        "Missing file-flush rejection test"
    );
}

void test_concurrent_no_replace_publication()
{
    constexpr std::size_t thread_count =
        12;

    TemporaryDirectory directory;

    const std::filesystem::path destination_path =
        directory.path()
        / "shared.data";

    const std::string data(
        8192,
        'N'
    );

    std::vector<
        std::filesystem::path
    > temporary_paths;

    temporary_paths.reserve(
        thread_count
    );

    for (
        std::size_t index = 0;
        index < thread_count;
        ++index
    )
    {
        const std::filesystem::path path =
            directory.path()
            / (
                "publisher-"
                + std::to_string(
                    index
                )
                + ".tmp"
            );

        write_file(
            path,
            data
        );

        temporary_paths.push_back(
            path
        );
    }

    std::barrier start_barrier{
        static_cast<std::ptrdiff_t>(
            thread_count
        )
    };

    std::atomic<std::size_t>
        publication_count{
            0
        };

    std::atomic<std::size_t>
        destination_exists_count{
            0
        };

    std::mutex exception_mutex;
    std::exception_ptr worker_exception;

    std::vector<std::thread> workers;

    workers.reserve(
        thread_count
    );

    for (
        const std::filesystem::path& path :
        temporary_paths
    )
    {
        workers.emplace_back(
            [
                &start_barrier,
                &publication_count,
                &destination_exists_count,
                &exception_mutex,
                &worker_exception,
                path,
                destination_path
            ]()
            {
                try
                {
                    start_barrier.
                        arrive_and_wait();

                    const DurablePublishResult result =
                        publish_file_durably(
                            path,
                            destination_path
                        );

                    if (
                        result ==
                        DurablePublishResult::published
                    )
                    {
                        publication_count.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    }
                    else
                    {
                        destination_exists_count.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    }
                }
                catch (...)
                {
                    const std::lock_guard lock{
                        exception_mutex
                    };

                    if (!worker_exception)
                    {
                        worker_exception =
                            std::current_exception();
                    }
                }
            }
        );
    }

    for (
        std::thread& worker :
        workers
    )
    {
        worker.join();
    }

    if (worker_exception)
    {
        std::rethrow_exception(
            worker_exception
        );
    }

    require_equal(
        publication_count.load(
            std::memory_order_relaxed
        ),
        static_cast<std::size_t>(
            1
        ),
        "Concurrent winning-publication test"
    );

    require_equal(
        destination_exists_count.load(
            std::memory_order_relaxed
        ),
        thread_count - 1,
        "Concurrent rejected-publication test"
    );

    require_equal(
        read_file(
            destination_path
        ),
        data,
        "Concurrent publication-content test"
    );
}

}

int main()
{
    try
    {
        test_file_flush();

        std::cout
            << "[PASS] Durable file flush\n";

        test_successful_publication();

        std::cout
            << "[PASS] Durable no-replace publication\n";

        test_existing_destination_protection();

        std::cout
            << "[PASS] Durable destination protection\n";

        test_path_validation();

        std::cout
            << "[PASS] Durable publication validation\n";

        test_concurrent_no_replace_publication();

        std::cout
            << "[PASS] Durable concurrent publication\n";

        std::cout
            << "All NexusFS durable-file tests passed.\n";

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
