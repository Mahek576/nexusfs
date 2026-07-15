#include "nexusfs/http/http_router.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

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

    response.keep_alive(
        keep_alive
    );

    response.body() =
        payload.dump();

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
                    "The requested HTTP method is not "
                    "supported for this route."
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

HttpRouter::Response make_internal_error_response(
    const HttpRouter::Request& request
)
{
    const nlohmann::ordered_json payload = {
        {
            "error",
            {
                {
                    "code",
                    "internal_server_error"
                },
                {
                    "message",
                    "The server could not complete "
                    "the requested operation."
                }
            }
        }
    };

    return make_json_response(
        beast_http::status::internal_server_error,
        payload,
        request.version(),
        request.keep_alive()
    );
}

HttpRouter::Response make_health_response(
    const HttpRouter::Request& request
)
{
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

HttpRouter::HttpRouter(
    std::shared_ptr<
        const app::NexusFsService
    > service
)
    : service_{std::move(service)}
{
    if (!service_)
    {
        throw std::invalid_argument(
            "HTTP router service cannot be null."
        );
    }
}

HttpRouter::Response HttpRouter::route(
    const Request& request
) const
{
    if (
        request.target() ==
        "/api/v1/health"
    )
    {
        if (
            request.method() !=
            beast_http::verb::get
        )
        {
            return make_method_not_allowed_response(
                request
            );
        }

        return make_health_response(
            request
        );
    }

    if (
        request.target() ==
        "/api/v1/files"
    )
    {
        if (
            request.method() !=
            beast_http::verb::get
        )
        {
            return make_method_not_allowed_response(
                request
            );
        }

        try
        {
            const app::ListFilesResult result =
                service_->list_files();

            nlohmann::ordered_json files =
                nlohmann::ordered_json::array();

            for (
                const app::StoredFileSummary& file :
                result.files
            )
            {
                files.push_back(
                    {
                        {
                            "manifest_id",
                            file.manifest_id
                        },
                        {
                            "filename",
                            file.original_filename
                        },
                        {
                            "file_size",
                            file.file_size
                        },
                        {
                            "chunk_size",
                            file.configured_chunk_size
                        },
                        {
                            "chunk_count",
                            file.chunk_count
                        },
                        {
                            "missing_chunks",
                            file.missing_chunks
                        },
                        {
                            "storage_status",
                            file.missing_chunks == 0
                                ? "complete"
                                : "incomplete"
                        }
                    }
                );
            }

            const nlohmann::ordered_json payload = {
                {
                    "files",
                    std::move(files)
                },
                {
                    "summary",
                    {
                        {
                            "stored_manifests",
                            result.files.size()
                        },
                        {
                            "complete_manifests",
                            result.complete_manifests
                        },
                        {
                            "incomplete_manifests",
                            result.incomplete_manifests
                        }
                    }
                }
            };

            return make_json_response(
                beast_http::status::ok,
                payload,
                request.version(),
                request.keep_alive()
            );
        }
        catch (const std::exception&)
        {
            return make_internal_error_response(
                request
            );
        }
    }

    return make_not_found_response(
        request
    );
}

}