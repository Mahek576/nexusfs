#include "nexusfs/http/http_router.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{

namespace beast_http = boost::beast::http;

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
        test_name + " content-length header test"
    );
}

void test_health_route()
{
    const nexusfs::http::HttpRouter router;

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
        "Health API-version body test"
    );

    require_equal(
        payload.size(),
        static_cast<std::size_t>(3),
        "Health JSON-field count test"
    );
}

void test_health_method_not_allowed()
{
    const nexusfs::http::HttpRouter router;

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

    require_true(
        !response.keep_alive(),
        "Method-not-allowed keep-alive test"
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

    require_true(
        !payload.at("error")
            .at("message")
            .get<std::string>()
            .empty(),
        "Method-not-allowed error-message test"
    );
}

void test_unknown_route()
{
    const nexusfs::http::HttpRouter router;

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::get,
        "/api/v1/unknown",
        10
    };

    request.keep_alive(false);

    const auto response =
        router.route(request);

    require_equal(
        response.result(),
        beast_http::status::not_found,
        "Unknown-route status test"
    );

    require_equal(
        response.version(),
        static_cast<unsigned int>(10),
        "Unknown-route HTTP-version test"
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

    require_true(
        !payload.at("error")
            .at("message")
            .get<std::string>()
            .empty(),
        "Unknown-route error-message test"
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