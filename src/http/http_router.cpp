#include "nexusfs/http/http_router.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace nexusfs::http
{

namespace beast_http = boost::beast::http;

namespace
{

constexpr std::string_view health_route{
    "/api/v1/health"
};

constexpr std::string_view files_route{
    "/api/v1/files"
};

constexpr std::string_view file_route_prefix{
    "/api/v1/files/"
};

constexpr std::string_view verify_route_suffix{
    "/verify"
};

std::string_view request_target(
    const HttpRouter::Request& request
)
{
    const auto target =
        request.target();

    return std::string_view{
        target.data(),
        target.size()
    };
}

bool is_valid_manifest_id(
    std::string_view manifest_id
)
{
    if (manifest_id.size() != 64)
    {
        return false;
    }

    return std::all_of(
        manifest_id.begin(),
        manifest_id.end(),
        [](char character)
        {
            return (
                character >= '0' &&
                character <= '9'
            ) || (
                character >= 'a' &&
                character <= 'f'
            );
        }
    );
}

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

HttpRouter::Response make_error_response(
    const HttpRouter::Request& request,
    beast_http::status status,
    std::string code,
    std::string message
)
{
    const nlohmann::ordered_json payload = {
        {
            "error",
            {
                {
                    "code",
                    std::move(code)
                },
                {
                    "message",
                    std::move(message)
                }
            }
        }
    };

    return make_json_response(
        status,
        payload,
        request.version(),
        request.keep_alive()
    );
}

HttpRouter::Response make_method_not_allowed_response(
    const HttpRouter::Request& request,
    std::string allowed_method
)
{
    HttpRouter::Response response =
        make_error_response(
            request,
            beast_http::status::method_not_allowed,
            "method_not_allowed",
            "The requested HTTP method is not "
            "supported for this route."
        );

    response.set(
        beast_http::field::allow,
        std::move(allowed_method)
    );

    return response;
}

HttpRouter::Response make_not_found_response(
    const HttpRouter::Request& request
)
{
    return make_error_response(
        request,
        beast_http::status::not_found,
        "not_found",
        "The requested API route was not found."
    );
}

HttpRouter::Response make_invalid_manifest_id_response(
    const HttpRouter::Request& request
)
{
    return make_error_response(
        request,
        beast_http::status::bad_request,
        "invalid_manifest_id",
        "The manifest ID must contain exactly "
        "64 lowercase hexadecimal characters."
    );
}

HttpRouter::Response make_manifest_not_found_response(
    const HttpRouter::Request& request
)
{
    return make_error_response(
        request,
        beast_http::status::not_found,
        "manifest_not_found",
        "The requested manifest was not found."
    );
}

HttpRouter::Response
make_integrity_verification_failed_response(
    const HttpRouter::Request& request
)
{
    return make_error_response(
        request,
        beast_http::status::conflict,
        "integrity_verification_failed",
        "One or more stored chunks failed "
        "integrity verification."
    );
}

HttpRouter::Response make_internal_error_response(
    const HttpRouter::Request& request
)
{
    return make_error_response(
        request,
        beast_http::status::internal_server_error,
        "internal_server_error",
        "The server could not complete "
        "the requested operation."
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

HttpRouter::Response make_files_response(
    const HttpRouter::Request& request,
    const app::ListFilesResult& result
)
{
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

HttpRouter::Response make_file_detail_response(
    const HttpRouter::Request& request,
    const app::InspectFileResult& result
)
{
    nlohmann::ordered_json chunks =
        nlohmann::ordered_json::array();

    for (
        const app::InspectedChunk& chunk :
        result.chunks
    )
    {
        chunks.push_back(
            {
                {
                    "index",
                    chunk.index
                },
                {
                    "hash",
                    chunk.hash
                },
                {
                    "present",
                    chunk.present
                }
            }
        );
    }

    const nlohmann::ordered_json payload = {
        {
            "file",
            {
                {
                    "manifest_id",
                    result.manifest_id
                },
                {
                    "filename",
                    result.original_filename
                },
                {
                    "file_size",
                    result.file_size
                },
                {
                    "chunk_size",
                    result.configured_chunk_size
                },
                {
                    "encoded_manifest_size",
                    result.encoded_manifest_size
                },
                {
                    "chunk_count",
                    result.chunks.size()
                },
                {
                    "available_chunks",
                    result.available_chunks
                },
                {
                    "missing_chunks",
                    result.missing_chunks
                },
                {
                    "storage_status",
                    result.missing_chunks == 0
                        ? "complete"
                        : "incomplete"
                }
            }
        },
        {
            "chunks",
            std::move(chunks)
        }
    };

    return make_json_response(
        beast_http::status::ok,
        payload,
        request.version(),
        request.keep_alive()
    );
}

HttpRouter::Response make_verification_response(
    const HttpRouter::Request& request,
    const app::VerifyFileResult& result
)
{
    nlohmann::ordered_json chunks =
        nlohmann::ordered_json::array();

    for (
        const app::VerifiedChunkResult& chunk :
        result.verified_chunks
    )
    {
        chunks.push_back(
            {
                {
                    "index",
                    chunk.index
                },
                {
                    "hash",
                    chunk.hash
                },
                {
                    "bytes_verified",
                    chunk.bytes_verified
                }
            }
        );
    }

    const nlohmann::ordered_json payload = {
        {
            "verification",
            {
                {
                    "manifest_id",
                    result.manifest_id
                },
                {
                    "filename",
                    result.original_filename
                },
                {
                    "file_size",
                    result.file_size
                },
                {
                    "chunk_count",
                    result.chunk_count
                },
                {
                    "verified_chunks",
                    result.verified_chunks.size()
                },
                {
                    "total_bytes_verified",
                    result.total_bytes_verified
                },
                {
                    "storage_integrity",
                    "healthy"
                }
            }
        },
        {
            "chunks",
            std::move(chunks)
        }
    };

    return make_json_response(
        beast_http::status::ok,
        payload,
        request.version(),
        request.keep_alive()
    );
}

bool catalog_contains_manifest(
    const app::ListFilesResult& catalog,
    const std::string& manifest_id
)
{
    return std::any_of(
        catalog.files.begin(),
        catalog.files.end(),
        [&manifest_id](
            const app::StoredFileSummary& file
        )
        {
            return (
                file.manifest_id ==
                manifest_id
            );
        }
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
    const std::string_view target =
        request_target(request);

    if (target == health_route)
    {
        if (
            request.method() !=
            beast_http::verb::get
        )
        {
            return make_method_not_allowed_response(
                request,
                "GET"
            );
        }

        return make_health_response(
            request
        );
    }

    if (target == files_route)
    {
        if (
            request.method() !=
            beast_http::verb::get
        )
        {
            return make_method_not_allowed_response(
                request,
                "GET"
            );
        }

        try
        {
            return make_files_response(
                request,
                service_->list_files()
            );
        }
        catch (const std::exception&)
        {
            return make_internal_error_response(
                request
            );
        }
    }

    if (target.starts_with(file_route_prefix))
    {
        std::string_view remaining_target =
            target.substr(
                file_route_prefix.size()
            );

        const bool is_verify_route =
            remaining_target.ends_with(
                verify_route_suffix
            );

        if (is_verify_route)
        {
            remaining_target.remove_suffix(
                verify_route_suffix.size()
            );

            if (
                request.method() !=
                beast_http::verb::post
            )
            {
                return make_method_not_allowed_response(
                    request,
                    "POST"
                );
            }
        }
        else
        {
            if (
                request.method() !=
                beast_http::verb::get
            )
            {
                return make_method_not_allowed_response(
                    request,
                    "GET"
                );
            }
        }

        if (
            !is_valid_manifest_id(
                remaining_target
            )
        )
        {
            return make_invalid_manifest_id_response(
                request
            );
        }

        const std::string manifest_id{
            remaining_target
        };

        try
        {
            const app::ListFilesResult catalog =
                service_->list_files();

            if (
                !catalog_contains_manifest(
                    catalog,
                    manifest_id
                )
            )
            {
                return make_manifest_not_found_response(
                    request
                );
            }

            if (is_verify_route)
            {
                try
                {
                    return make_verification_response(
                        request,
                        service_->verify_file(
                            manifest_id
                        )
                    );
                }
                catch (const std::exception&)
                {
                    return
                        make_integrity_verification_failed_response(
                            request
                        );
                }
            }

            return make_file_detail_response(
                request,
                service_->inspect_file(
                    manifest_id
                )
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