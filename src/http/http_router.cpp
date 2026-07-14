#include "nexusfs/http/http_router.hpp"

#include <nlohmann/json.hpp>

namespace nexusfs::http
{

namespace beast_http = boost::beast::http;

namespace
{

HttpRouter::Response make_json_response(
    beast_http::status status,
    const nlohmann::ordered_json& payload,
    unsigned int http_version,
    bool keep_alive
)
{
    HttpRouter::Response response{
        status,
        http_version
    };

    response.set(
        beast_http::field::server,
        "NexusFS"
    );

    response.set(
        beast_http::field::content_type,
        "application/json"
    );

    response.set(
        beast_http::field::cache_control,
        "no-store"
    );

    response.keep_alive(keep_alive);
    response.body() = payload.dump();
    response.prepare_payload();

    return response;
}

HttpRouter::Response make_method_not_allowed_response(
    const HttpRouter::Request& request
)
{
    const nlohmann::ordered_json payload = {
        {
            "error",
            {
                {
                    "code",
                    "method_not_allowed"
                },
                {
                    "message",
                    "The requested HTTP method is not supported "
                    "for this route."
                }
            }
        }
    };

    HttpRouter::Response response =
        make_json_response(
            beast_http::status::method_not_allowed,
            payload,
            request.version(),
            request.keep_alive()
        );

    response.set(
        beast_http::field::allow,
        "GET"
    );

    return response;
}

HttpRouter::Response make_not_found_response(
    const HttpRouter::Request& request
)
{
    const nlohmann::ordered_json payload = {
        {
            "error",
            {
                {
                    "code",
                    "not_found"
                },
                {
                    "message",
                    "The requested API route was not found."
                }
            }
        }
    };

    return make_json_response(
        beast_http::status::not_found,
        payload,
        request.version(),
        request.keep_alive()
    );
}

}

HttpRouter::Response HttpRouter::route(
    const Request& request
) const
{
    if (request.target() != "/api/v1/health")
    {
        return make_not_found_response(
            request
        );
    }

    if (request.method() != beast_http::verb::get)
    {
        return make_method_not_allowed_response(
            request
        );
    }

    const nlohmann::ordered_json payload = {
        {
            "status",
            "healthy"
        },
        {
            "service",
            "nexusfs"
        },
        {
            "api_version",
            "v1"
        }
    };

    return make_json_response(
        beast_http::status::ok,
        payload,
        request.version(),
        request.keep_alive()
    );
}

}