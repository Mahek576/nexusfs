#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/http/http_router.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

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
#include <vector>

namespace
{

namespace beast_http = boost::beast::http;

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
                "nexusfs-http-tests-"
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
                "Failed to create HTTP test directory: "
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
        data[index] =
            static_cast<std::uint8_t>(
                (
                    index * 17U
                    + 29U
                )
                % 256U
            );
    }

    return data;
}

void write_test_file(
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
                "Failed to create HTTP test file directory: "
                + directory_error.message()
            );
        }
    }

    std::ofstream output{
        path,
        std::ios::binary | std::ios::trunc
    };

    if (!output.is_open())
    {
        throw std::runtime_error(
            "Failed to create HTTP test source file."
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
            "Failed while writing HTTP test source file."
        );
    }
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

std::filesystem::path chunk_path(
    const std::filesystem::path& storage_root,
    const std::string& chunk_hash
)
{
    if (chunk_hash.size() != 64)
    {
        throw std::runtime_error(
            "Cannot construct a chunk path from "
            "an invalid chunk hash."
        );
    }

    return storage_root
        / "chunks"
        / chunk_hash.substr(0, 2)
        / chunk_hash.substr(2);
}

void corrupt_first_byte(
    const std::filesystem::path& path
)
{
    std::fstream file{
        path,
        std::ios::binary
            | std::ios::in
            | std::ios::out
    };

    if (!file.is_open())
    {
        throw std::runtime_error(
            "Failed to open stored chunk for corruption test."
        );
    }

    char original_byte = '\0';

    file.read(
        &original_byte,
        1
    );

    if (!file)
    {
        throw std::runtime_error(
            "Failed to read stored chunk during corruption test."
        );
    }

    const auto unsigned_original =
        static_cast<unsigned char>(
            original_byte
        );

    const char corrupted_byte =
        static_cast<char>(
            unsigned_original ^ 0xFFU
        );

    file.clear();

    file.seekp(
        0,
        std::ios::beg
    );

    file.write(
        &corrupted_byte,
        1
    );

    file.flush();

    if (!file)
    {
        throw std::runtime_error(
            "Failed to corrupt stored chunk."
        );
    }
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

void require_common_json_headers(
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

void test_health_route()
{
    TemporaryDirectory directory;

    const nexusfs::http::HttpRouter router{
        create_service(
            directory.path() / "storage"
        )
    };

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::get,
        "/api/v1/health",
        11
    };

    request.keep_alive(true);

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::ok,
        "Health status test"
    );

    require_true(
        response.keep_alive(),
        "Health keep-alive test"
    );

    require_common_json_headers(
        response,
        "Health response"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    require_equal(
        payload.at("status").get<std::string>(),
        std::string{"healthy"},
        "Health status-body test"
    );

    require_equal(
        payload.at("service").get<std::string>(),
        std::string{"nexusfs"},
        "Health service-body test"
    );

    require_equal(
        payload.at("api_version").get<std::string>(),
        std::string{"v1"},
        "Health API-version test"
    );
}

void test_health_method_not_allowed()
{
    TemporaryDirectory directory;

    const nexusfs::http::HttpRouter router{
        create_service(
            directory.path() / "storage"
        )
    };

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::post,
        "/api/v1/health",
        11
    };

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::method_not_allowed,
        "Method-not-allowed status test"
    );

    require_equal(
        header_value(
            response,
            beast_http::field::allow
        ),
        std::string{"GET"},
        "Method-not-allowed Allow-header test"
    );
}

void test_unknown_route()
{
    TemporaryDirectory directory;

    const nexusfs::http::HttpRouter router{
        create_service(
            directory.path() / "storage"
        )
    };

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::get,
        "/api/v1/unknown",
        11
    };

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::not_found,
        "Unknown-route status test"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    require_equal(
        payload.at("error")
            .at("code")
            .get<std::string>(),
        std::string{"not_found"},
        "Unknown-route error-code test"
    );
}

void test_files_catalog_route()
{
    TemporaryDirectory directory;

    const std::filesystem::path source_path =
        directory.path() / "catalog.bin";

    write_test_file(
        source_path,
        create_test_data(1500)
    );

    const auto service =
        create_service(
            directory.path() / "storage"
        );

    const auto stored =
        service->store_file(
            source_path
        );

    const nexusfs::http::HttpRouter router{
        service
    };

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::get,
        "/api/v1/files",
        11
    };

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::ok,
        "Files catalog status test"
    );

    require_common_json_headers(
        response,
        "Files catalog response"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    require_equal(
        payload.at("files").size(),
        static_cast<std::size_t>(1),
        "Files catalog count test"
    );

    require_equal(
        payload.at("files")
            .at(0)
            .at("manifest_id")
            .get<std::string>(),
        stored.manifest_id,
        "Files catalog manifest-ID test"
    );

    require_equal(
        payload.at("summary")
            .at("stored_manifests")
            .get<std::size_t>(),
        static_cast<std::size_t>(1),
        "Files catalog summary test"
    );
}

void test_file_detail_route()
{
    TemporaryDirectory directory;

    const std::filesystem::path source_path =
        directory.path() / "detail.bin";

    write_test_file(
        source_path,
        create_test_data(2500)
    );

    const auto service =
        create_service(
            directory.path() / "storage"
        );

    const auto stored =
        service->store_file(
            source_path
        );

    const nexusfs::http::HttpRouter router{
        service
    };

    const std::string target =
        "/api/v1/files/"
        + stored.manifest_id;

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::get,
        target,
        11
    };

    request.keep_alive(true);

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::ok,
        "File detail status test"
    );

    require_true(
        response.keep_alive(),
        "File detail keep-alive test"
    );

    require_common_json_headers(
        response,
        "File detail response"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    const nlohmann::json& file =
        payload.at("file");

    require_equal(
        file.at("manifest_id").get<std::string>(),
        stored.manifest_id,
        "File detail manifest-ID test"
    );

    require_equal(
        file.at("filename").get<std::string>(),
        std::string{"detail.bin"},
        "File detail filename test"
    );

    require_equal(
        file.at("file_size").get<std::uint64_t>(),
        static_cast<std::uint64_t>(2500),
        "File detail size test"
    );

    require_equal(
        file.at("chunk_count").get<std::size_t>(),
        static_cast<std::size_t>(3),
        "File detail chunk-count test"
    );

    require_equal(
        file.at("available_chunks").get<std::size_t>(),
        static_cast<std::size_t>(3),
        "File detail available-chunks test"
    );

    require_equal(
        file.at("missing_chunks").get<std::size_t>(),
        static_cast<std::size_t>(0),
        "File detail missing-chunks test"
    );

    require_equal(
        file.at("storage_status").get<std::string>(),
        std::string{"complete"},
        "File detail storage-status test"
    );

    require_equal(
        payload.at("chunks").size(),
        static_cast<std::size_t>(3),
        "File detail chunk-array test"
    );

    for (
        std::size_t index = 0;
        index < payload.at("chunks").size();
        ++index
    )
    {
        const nlohmann::json& chunk =
            payload.at("chunks").at(index);

        require_equal(
            chunk.at("index").get<std::size_t>(),
            index,
            "File detail chunk-index test"
        );

        require_equal(
            chunk.at("hash")
                .get<std::string>()
                .size(),
            static_cast<std::size_t>(64),
            "File detail chunk-hash test"
        );

        require_true(
            chunk.at("present").get<bool>(),
            "File detail chunk-presence test"
        );
    }
}

void test_invalid_manifest_id_route()
{
    TemporaryDirectory directory;

    const nexusfs::http::HttpRouter router{
        create_service(
            directory.path() / "storage"
        )
    };

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::get,
        "/api/v1/files/invalid-id",
        11
    };

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::bad_request,
        "Invalid manifest-ID status test"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    require_equal(
        payload.at("error")
            .at("code")
            .get<std::string>(),
        std::string{"invalid_manifest_id"},
        "Invalid manifest-ID error-code test"
    );
}

void test_missing_manifest_route()
{
    TemporaryDirectory directory;

    const nexusfs::http::HttpRouter router{
        create_service(
            directory.path() / "storage"
        )
    };

    const std::string missing_manifest_id(
        64,
        'f'
    );

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::get,
        "/api/v1/files/"
            + missing_manifest_id,
        11
    };

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::not_found,
        "Missing manifest status test"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    require_equal(
        payload.at("error")
            .at("code")
            .get<std::string>(),
        std::string{"manifest_not_found"},
        "Missing manifest error-code test"
    );
}

void test_verification_route()
{
    TemporaryDirectory directory;

    const std::filesystem::path source_path =
        directory.path() / "verify.bin";

    write_test_file(
        source_path,
        create_test_data(2500)
    );

    const auto service =
        create_service(
            directory.path() / "storage"
        );

    const auto stored =
        service->store_file(
            source_path
        );

    const nexusfs::http::HttpRouter router{
        service
    };

    const std::string target =
        "/api/v1/files/"
        + stored.manifest_id
        + "/verify";

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::post,
        target,
        11
    };

    request.keep_alive(true);

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::ok,
        "Verification status test"
    );

    require_true(
        response.keep_alive(),
        "Verification keep-alive test"
    );

    require_common_json_headers(
        response,
        "Verification response"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    const nlohmann::json& verification =
        payload.at("verification");

    require_equal(
        verification.at("manifest_id")
            .get<std::string>(),
        stored.manifest_id,
        "Verification manifest-ID test"
    );

    require_equal(
        verification.at("filename")
            .get<std::string>(),
        std::string{"verify.bin"},
        "Verification filename test"
    );

    require_equal(
        verification.at("file_size")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(2500),
        "Verification file-size test"
    );

    require_equal(
        verification.at("chunk_count")
            .get<std::size_t>(),
        static_cast<std::size_t>(3),
        "Verification chunk-count test"
    );

    require_equal(
        verification.at("verified_chunks")
            .get<std::size_t>(),
        static_cast<std::size_t>(3),
        "Verification verified-chunks test"
    );

    require_equal(
        verification.at("total_bytes_verified")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(2500),
        "Verification byte-count test"
    );

    require_equal(
        verification.at("storage_integrity")
            .get<std::string>(),
        std::string{"healthy"},
        "Verification integrity-status test"
    );

    require_equal(
        payload.at("chunks").size(),
        static_cast<std::size_t>(3),
        "Verification chunk-array test"
    );

    require_equal(
        payload.at("chunks")
            .at(0)
            .at("bytes_verified")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1024),
        "Verification first-chunk size test"
    );

    require_equal(
        payload.at("chunks")
            .at(1)
            .at("bytes_verified")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(1024),
        "Verification second-chunk size test"
    );

    require_equal(
        payload.at("chunks")
            .at(2)
            .at("bytes_verified")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(452),
        "Verification final-chunk size test"
    );
}

void test_verification_method_not_allowed()
{
    TemporaryDirectory directory;

    const nexusfs::http::HttpRouter router{
        create_service(
            directory.path() / "storage"
        )
    };

    const std::string manifest_id(
        64,
        'a'
    );

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::get,
        "/api/v1/files/"
            + manifest_id
            + "/verify",
        11
    };

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::method_not_allowed,
        "Verification method status test"
    );

    require_equal(
        header_value(
            response,
            beast_http::field::allow
        ),
        std::string{"POST"},
        "Verification Allow-header test"
    );
}

void test_missing_manifest_verification()
{
    TemporaryDirectory directory;

    const nexusfs::http::HttpRouter router{
        create_service(
            directory.path() / "storage"
        )
    };

    const std::string missing_manifest_id(
        64,
        'f'
    );

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::post,
        "/api/v1/files/"
            + missing_manifest_id
            + "/verify",
        11
    };

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::not_found,
        "Missing verification manifest status test"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    require_equal(
        payload.at("error")
            .at("code")
            .get<std::string>(),
        std::string{"manifest_not_found"},
        "Missing verification manifest code test"
    );
}

void test_corrupted_chunk_verification()
{
    TemporaryDirectory directory;

    const std::filesystem::path storage_root =
        directory.path() / "storage";

    const std::filesystem::path source_path =
        directory.path() / "corrupt.bin";

    write_test_file(
        source_path,
        create_test_data(1500)
    );

    const auto service =
        create_service(
            storage_root
        );

    const auto stored =
        service->store_file(
            source_path
        );

    const auto inspected =
        service->inspect_file(
            stored.manifest_id
        );

    require_true(
        !inspected.chunks.empty(),
        "Corruption test inspected-chunk test"
    );

    const std::filesystem::path stored_chunk_path =
        chunk_path(
            storage_root,
            inspected.chunks.front().hash
        );

    corrupt_first_byte(
        stored_chunk_path
    );

    const nexusfs::http::HttpRouter router{
        service
    };

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::post,
        "/api/v1/files/"
            + stored.manifest_id
            + "/verify",
        11
    };

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::conflict,
        "Corrupted verification status test"
    );

    require_common_json_headers(
        response,
        "Corrupted verification response"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    require_equal(
        payload.at("error")
            .at("code")
            .get<std::string>(),
        std::string{
            "integrity_verification_failed"
        },
        "Corrupted verification error-code test"
    );
}

}

int main()
{
    try
    {
        test_health_route();

        std::cout
            << "[PASS] HTTP health route\n";

        test_health_method_not_allowed();

        std::cout
            << "[PASS] HTTP method-not-allowed response\n";

        test_unknown_route();

        std::cout
            << "[PASS] HTTP unknown-route response\n";

        test_files_catalog_route();

        std::cout
            << "[PASS] HTTP files catalog route\n";

        test_file_detail_route();

        std::cout
            << "[PASS] HTTP file detail route\n";

        test_invalid_manifest_id_route();

        std::cout
            << "[PASS] HTTP invalid manifest ID response\n";

        test_missing_manifest_route();

        std::cout
            << "[PASS] HTTP missing manifest response\n";

        test_verification_route();

        std::cout
            << "[PASS] HTTP verification route\n";

        test_verification_method_not_allowed();

        std::cout
            << "[PASS] HTTP verification method rejection\n";

        test_missing_manifest_verification();

        std::cout
            << "[PASS] HTTP missing verification manifest\n";

        test_corrupted_chunk_verification();

        std::cout
            << "[PASS] HTTP corrupted chunk verification\n";

        std::cout
            << "All NexusFS HTTP router tests passed.\n";

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