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

void write_test_file(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& data
)
{
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

void test_health_route()
{
    TemporaryDirectory directory;

    const auto service =
        create_service(
            directory.path() / "storage"
        );

    const nexusfs::http::HttpRouter router{
        service
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

    require_equal(
        response.version(),
        static_cast<unsigned int>(11),
        "Health HTTP-version test"
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

    const auto service =
        create_service(
            directory.path() / "storage"
        );

    const nexusfs::http::HttpRouter router{
        service
    };

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::post,
        "/api/v1/health",
        11
    };

    request.keep_alive(false);

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::method_not_allowed,
        "Method-not-allowed status test"
    );

    require_common_json_headers(
        response,
        "Method-not-allowed response"
    );

    require_equal(
        header_value(
            response,
            beast_http::field::allow
        ),
        std::string{"GET"},
        "Method-not-allowed Allow-header test"
    );

    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body()
        );

    require_equal(
        payload.at("error")
            .at("code")
            .get<std::string>(),
        std::string{"method_not_allowed"},
        "Method-not-allowed error-code test"
    );
}

void test_unknown_route()
{
    TemporaryDirectory directory;

    const auto service =
        create_service(
            directory.path() / "storage"
        );

    const nexusfs::http::HttpRouter router{
        service
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

    require_common_json_headers(
        response,
        "Unknown-route response"
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

    const std::filesystem::path storage_root =
        directory.path() / "storage";

    const std::filesystem::path source_path =
        directory.path() / "catalog.bin";

    std::vector<std::uint8_t> source_data(
        1500
    );

    for (
        std::size_t index = 0;
        index < source_data.size();
        ++index
    )
    {
        source_data[index] =
            static_cast<std::uint8_t>(
                index % 251
            );
    }

    write_test_file(
        source_path,
        source_data
    );

    const auto service =
        create_service(
            storage_root
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

    request.keep_alive(true);

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::ok,
        "Files catalog status test"
    );

    require_true(
        response.keep_alive(),
        "Files catalog keep-alive test"
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
        "Files catalog entry-count test"
    );

    const nlohmann::json& file =
        payload.at("files").at(0);

    require_equal(
        file.at("manifest_id").get<std::string>(),
        stored.manifest_id,
        "Files catalog manifest-ID test"
    );

    require_equal(
        file.at("filename").get<std::string>(),
        std::string{"catalog.bin"},
        "Files catalog filename test"
    );

    require_equal(
        file.at("file_size").get<std::uint64_t>(),
        static_cast<std::uint64_t>(1500),
        "Files catalog file-size test"
    );

    require_equal(
        file.at("chunk_size").get<std::size_t>(),
        static_cast<std::size_t>(1024),
        "Files catalog chunk-size test"
    );

    require_equal(
        file.at("chunk_count").get<std::size_t>(),
        static_cast<std::size_t>(2),
        "Files catalog chunk-count test"
    );

    require_equal(
        file.at("missing_chunks").get<std::size_t>(),
        static_cast<std::size_t>(0),
        "Files catalog missing-chunk test"
    );

    require_equal(
        file.at("storage_status").get<std::string>(),
        std::string{"complete"},
        "Files catalog storage-status test"
    );

    const nlohmann::json& summary =
        payload.at("summary");

    require_equal(
        summary.at("stored_manifests")
            .get<std::size_t>(),
        static_cast<std::size_t>(1),
        "Files summary stored-manifest test"
    );

    require_equal(
        summary.at("complete_manifests")
            .get<std::size_t>(),
        static_cast<std::size_t>(1),
        "Files summary complete-manifest test"
    );

    require_equal(
        summary.at("incomplete_manifests")
            .get<std::size_t>(),
        static_cast<std::size_t>(0),
        "Files summary incomplete-manifest test"
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