#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/http/http_router.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

namespace beast_http = boost::beast::http;

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
                "nexusfs-http-upload-tests-"
                + std::to_string(timestamp)
                + "-"
                + std::to_string(current_sequence)
            );

        std::error_code error;

        std::filesystem::create_directories(
            path_,
            error
        );

        if (error)
        {
            throw std::runtime_error(
                "Failed to create upload test directory: "
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

std::vector<std::uint8_t> create_test_data(
    std::size_t size
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
        const std::size_t block_number =
            index / 256U;

        data[index] =
            static_cast<std::uint8_t>(
                (
                    index * 37U
                    + block_number * 17U
                    + 19U
                )
                % 256U
            );
    }

    return data;
}

std::string binary_body(
    const std::vector<std::uint8_t>& data
)
{
    if (data.empty())
    {
        return {};
    }

    return std::string{
        reinterpret_cast<const char*>(
            data.data()
        ),
        data.size()
    };
}

std::vector<std::uint8_t> read_binary_file(
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
            "Failed to open restored upload test file."
        );
    }

    input.seekg(
        0,
        std::ios::end
    );

    const std::streampos end_position =
        input.tellg();

    if (end_position < 0)
    {
        throw std::runtime_error(
            "Failed to determine restored upload file size."
        );
    }

    input.seekg(
        0,
        std::ios::beg
    );

    std::vector<std::uint8_t> data(
        static_cast<std::size_t>(
            end_position
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
    }

    if (!input && !data.empty())
    {
        throw std::runtime_error(
            "Failed while reading restored upload test file."
        );
    }

    return data;
}

std::shared_ptr<nexusfs::app::NexusFsService>
create_service(
    const std::filesystem::path& storage_root
)
{
    return std::make_shared<
        nexusfs::app::NexusFsService
    >(
        storage_root,
        1024
    );
}

nexusfs::http::HttpRouter::Request
make_upload_request(
    const std::string& filename,
    const std::vector<std::uint8_t>& data,
    const std::string& content_type =
        "application/octet-stream"
)
{
    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::post,
        "/api/v1/files",
        11
    };

    request.set(
        beast_http::field::content_type,
        content_type
    );

    request.set(
        "X-NexusFS-Filename",
        filename
    );

    request.body() =
        binary_body(
            data
        );

    request.prepare_payload();
    request.keep_alive(true);

    return request;
}

std::string header_value(
    const nexusfs::http::HttpRouter::Response& response,
    beast_http::field field
)
{
    return std::string{
        response[field]
    };
}

void require_common_headers(
    const nexusfs::http::HttpRouter::Response& response,
    const std::string& test_name
)
{
    require_equal(
        header_value(
            response,
            beast_http::field::server
        ),
        std::string{"NexusFS"},
        test_name + " server-header test"
    );

    require_equal(
        header_value(
            response,
            beast_http::field::content_type
        ),
        std::string{"application/json"},
        test_name + " content-type test"
    );

    require_equal(
        header_value(
            response,
            beast_http::field::cache_control
        ),
        std::string{"no-store"},
        test_name + " cache-control test"
    );

    require_true(
        response.find(
            beast_http::field::content_length
        ) != response.end(),
        test_name + " content-length test"
    );
}

void require_error_code(
    const nexusfs::http::HttpRouter::Response& response,
    beast_http::status expected_status,
    const std::string& expected_code,
    const std::string& test_name
)
{
    require_equal(
        response.result(),
        expected_status,
        test_name + " status test"
    );

    require_common_headers(
        response,
        test_name + " response"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    require_equal(
        payload.at("error")
            .at("code")
            .get<std::string>(),
        expected_code,
        test_name + " error-code test"
    );
}

void test_binary_upload()
{
    TemporaryDirectory directory;

    const std::filesystem::path storage_root =
        directory.path()
        / "storage";

    const auto service =
        create_service(
            storage_root
        );

    const nexusfs::http::HttpRouter router{
        service
    };

    const std::vector<std::uint8_t> source_data =
        create_test_data(
            2500
        );

    const auto request =
        make_upload_request(
            "uploaded.bin",
            source_data,
            "Application/Octet-Stream; charset=binary"
        );

    const auto response =
        router.route(
            request
        );

    require_equal(
        response.result(),
        beast_http::status::created,
        "Binary upload status test"
    );

    require_true(
        response.keep_alive(),
        "Binary upload keep-alive test"
    );

    require_common_headers(
        response,
        "Binary upload response"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    const nlohmann::json& stored_file =
        payload.at(
            "stored_file"
        );

    const std::string manifest_id =
        stored_file.at(
            "manifest_id"
        ).get<std::string>();

    require_equal(
        manifest_id.size(),
        static_cast<std::size_t>(64),
        "Binary upload manifest-ID length test"
    );

    require_equal(
        stored_file.at("filename")
            .get<std::string>(),
        std::string{"uploaded.bin"},
        "Binary upload filename test"
    );

    require_equal(
        stored_file.at("file_size")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(2500),
        "Binary upload file-size test"
    );

    require_equal(
        stored_file.at("chunk_count")
            .get<std::size_t>(),
        static_cast<std::size_t>(3),
        "Binary upload chunk-count test"
    );

    require_equal(
        stored_file.at("chunks_stored")
            .get<std::size_t>(),
        static_cast<std::size_t>(3),
        "Binary upload stored-chunks test"
    );

    require_equal(
        stored_file.at("chunks_reused")
            .get<std::size_t>(),
        static_cast<std::size_t>(0),
        "Binary upload reused-chunks test"
    );

    require_equal(
        stored_file.at("bytes_processed")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(2500),
        "Binary upload processed-bytes test"
    );

    require_true(
        stored_file.at("encoded_manifest_size")
            .get<std::size_t>() > 0,
        "Binary upload encoded-manifest-size test"
    );

    require_true(
        stored_file.at("manifest_stored")
            .get<bool>(),
        "Binary upload manifest-stored test"
    );

    require_equal(
        stored_file.at("status")
            .get<std::string>(),
        std::string{"stored"},
        "Binary upload result-status test"
    );

    const std::filesystem::path restored_path =
        directory.path()
        / "restored"
        / "uploaded.bin";

    const auto restoration =
        service->restore_file(
            manifest_id,
            restored_path
        );

    require_equal(
        restoration.bytes_written,
        static_cast<std::uint64_t>(2500),
        "Binary upload restoration size test"
    );

    require_equal(
        read_binary_file(
            restored_path
        ),
        source_data,
        "Binary upload byte-perfect test"
    );

    require_equal(
        service->list_files().files.size(),
        static_cast<std::size_t>(1),
        "Binary upload catalog-count test"
    );
}

void test_empty_upload()
{
    TemporaryDirectory directory;

    const auto service =
        create_service(
            directory.path()
            / "storage"
        );

    const nexusfs::http::HttpRouter router{
        service
    };

    const auto request =
        make_upload_request(
            "empty-upload.bin",
            {}
        );

    const auto response =
        router.route(
            request
        );

    require_equal(
        response.result(),
        beast_http::status::created,
        "Empty upload status test"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    const nlohmann::json& stored_file =
        payload.at(
            "stored_file"
        );

    require_equal(
        stored_file.at("file_size")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(0),
        "Empty upload file-size test"
    );

    require_equal(
        stored_file.at("chunk_count")
            .get<std::size_t>(),
        static_cast<std::size_t>(0),
        "Empty upload chunk-count test"
    );

    require_equal(
        stored_file.at("chunks_stored")
            .get<std::size_t>(),
        static_cast<std::size_t>(0),
        "Empty upload stored-chunks test"
    );

    require_equal(
        stored_file.at("chunks_reused")
            .get<std::size_t>(),
        static_cast<std::size_t>(0),
        "Empty upload reused-chunks test"
    );
}

void test_duplicate_upload()
{
    TemporaryDirectory directory;

    const auto service =
        create_service(
            directory.path()
            / "storage"
        );

    const nexusfs::http::HttpRouter router{
        service
    };

    const std::vector<std::uint8_t> data =
        create_test_data(
            1500
        );

    const auto first_response =
        router.route(
            make_upload_request(
                "duplicate.bin",
                data
            )
        );

    require_equal(
        first_response.result(),
        beast_http::status::created,
        "First duplicate upload status test"
    );

    const nlohmann::json first_payload =
        nlohmann::json::parse(
            first_response.body()
        );

    const std::string first_manifest_id =
        first_payload.at("stored_file")
            .at("manifest_id")
            .get<std::string>();

    const auto second_response =
        router.route(
            make_upload_request(
                "duplicate.bin",
                data
            )
        );

    require_equal(
        second_response.result(),
        beast_http::status::ok,
        "Repeated upload status test"
    );

    const nlohmann::json second_payload =
        nlohmann::json::parse(
            second_response.body()
        );

    const nlohmann::json& stored_file =
        second_payload.at(
            "stored_file"
        );

    require_equal(
        stored_file.at("manifest_id")
            .get<std::string>(),
        first_manifest_id,
        "Repeated upload manifest-ID test"
    );

    require_true(
        !stored_file.at("manifest_stored")
            .get<bool>(),
        "Repeated upload manifest-reuse test"
    );

    require_equal(
        stored_file.at("chunks_stored")
            .get<std::size_t>(),
        static_cast<std::size_t>(0),
        "Repeated upload stored-chunks test"
    );

    require_equal(
        stored_file.at("chunks_reused")
            .get<std::size_t>(),
        static_cast<std::size_t>(2),
        "Repeated upload reused-chunks test"
    );

    require_equal(
        stored_file.at("status")
            .get<std::string>(),
        std::string{"reused"},
        "Repeated upload result-status test"
    );

    require_equal(
        service->list_files().files.size(),
        static_cast<std::size_t>(1),
        "Repeated upload catalog deduplication test"
    );
}

void test_unsupported_content_type()
{
    TemporaryDirectory directory;

    const nexusfs::http::HttpRouter router{
        create_service(
            directory.path()
            / "storage"
        )
    };

    const auto response =
        router.route(
            make_upload_request(
                "wrong-type.bin",
                create_test_data(32),
                "application/json"
            )
        );

    require_error_code(
        response,
        beast_http::status::unsupported_media_type,
        "unsupported_media_type",
        "Unsupported upload media type"
    );
}

void test_missing_filename_header()
{
    TemporaryDirectory directory;

    const nexusfs::http::HttpRouter router{
        create_service(
            directory.path()
            / "storage"
        )
    };

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::post,
        "/api/v1/files",
        11
    };

    request.set(
        beast_http::field::content_type,
        "application/octet-stream"
    );

    request.body() = "test";
    request.prepare_payload();

    const auto response =
        router.route(
            request
        );

    require_error_code(
        response,
        beast_http::status::bad_request,
        "invalid_upload_filename",
        "Missing upload filename"
    );
}

void test_unsafe_upload_filenames()
{
    TemporaryDirectory directory;

    const auto service =
        create_service(
            directory.path()
            / "storage"
        );

    const nexusfs::http::HttpRouter router{
        service
    };

    std::vector<
        std::pair<std::string, std::string>
    > invalid_filenames{
        {
            "parent traversal",
            "../escape.bin"
        },
        {
            "forward-slash path",
            "folder/file.bin"
        },
        {
            "backslash path",
            "folder\\file.bin"
        },
        {
            "drive path",
            "C:drive.bin"
        },
        {
            "question mark",
            "bad?.bin"
        },
        {
            "asterisk",
            "bad*.bin"
        },
        {
            "less-than symbol",
            "bad<name.bin"
        },
        {
            "greater-than symbol",
            "bad>name.bin"
        },
        {
            "quote symbol",
            "bad\"name.bin"
        },
        {
            "pipe symbol",
            "bad|name.bin"
        },
        {
            "current directory",
            "."
        },
        {
            "parent directory",
            ".."
        },
        {
            "reserved CON name",
            "CON.txt"
        },
        {
            "reserved AUX name",
            "AUX"
        },
        {
            "reserved NUL name",
            "NUL.data"
        },
        {
            "reserved COM name",
            "COM1.bin"
        },
        {
            "reserved LPT name",
            "LPT1.bin"
        },
        {
            "trailing dot",
            "trailing."
        }
    };

    invalid_filenames.push_back(
        {
            "filename exceeding 255 bytes",
            std::string(
                256,
                'a'
            )
        }
    );

    for (
        const auto& [description, filename] :
        invalid_filenames
    )
    {
        const auto response =
            router.route(
                make_upload_request(
                    filename,
                    create_test_data(16)
                )
            );

        require_error_code(
            response,
            beast_http::status::bad_request,
            "invalid_upload_filename",
            "Unsafe upload filename: "
                + description
        );
    }

    require_true(
        service->list_files().files.empty(),
        "Unsafe upload storage-isolation test"
    );
}

void test_upload_method_not_allowed()
{
    TemporaryDirectory directory;

    const nexusfs::http::HttpRouter router{
        create_service(
            directory.path()
            / "storage"
        )
    };

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::delete_,
        "/api/v1/files",
        11
    };

    const auto response =
        router.route(
            request
        );

    require_error_code(
        response,
        beast_http::status::method_not_allowed,
        "method_not_allowed",
        "Upload route method rejection"
    );

    require_equal(
        header_value(
            response,
            beast_http::field::allow
        ),
        std::string{"GET, POST"},
        "Upload route Allow-header test"
    );
}

}

int main()
{
    try
    {
        test_binary_upload();

        std::cout
            << "[PASS] HTTP binary upload\n";

        test_empty_upload();

        std::cout
            << "[PASS] HTTP empty-file upload\n";

        test_duplicate_upload();

        std::cout
            << "[PASS] HTTP duplicate upload deduplication\n";

        test_unsupported_content_type();

        std::cout
            << "[PASS] HTTP upload media-type rejection\n";

        test_missing_filename_header();

        std::cout
            << "[PASS] HTTP missing upload filename\n";

        test_unsafe_upload_filenames();

        std::cout
            << "[PASS] HTTP unsafe upload filename rejection\n";

        test_upload_method_not_allowed();

        std::cout
            << "[PASS] HTTP upload method rejection\n";

        std::cout
            << "All NexusFS HTTP upload tests passed.\n";

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