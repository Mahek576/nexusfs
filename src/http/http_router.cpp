#include "nexusfs/http/http_router.hpp"

#include <boost/beast/core/string.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace nexusfs::http
{

namespace beast = boost::beast;
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

constexpr std::string_view restore_route_suffix{
    "/restore"
};

constexpr std::string_view upload_filename_header{
    "X-NexusFS-Filename"
};

constexpr std::string_view binary_content_type{
    "application/octet-stream"
};

constexpr std::size_t maximum_filename_length =
    255;

std::atomic<std::uint64_t> upload_sequence{
    0
};

class TemporaryUploadFile final
{
public:
    explicit TemporaryUploadFile(
        const std::string& filename
    )
        : directory_{
            create_unique_directory()
        }
    {
        try
        {
            file_path_ =
                directory_
                / filename;
        }
        catch (...)
        {
            cleanup();
            throw;
        }
    }

    TemporaryUploadFile(
        const TemporaryUploadFile&
    ) = delete;

    TemporaryUploadFile& operator=(
        const TemporaryUploadFile&
    ) = delete;

    TemporaryUploadFile(
        TemporaryUploadFile&&
    ) = delete;

    TemporaryUploadFile& operator=(
        TemporaryUploadFile&&
    ) = delete;

    ~TemporaryUploadFile()
    {
        cleanup();
    }

    void write(
        const std::string& body
    ) const
    {
        if (
            body.size() >
            static_cast<std::size_t>(
                std::numeric_limits<
                    std::streamsize
                >::max()
            )
        )
        {
            throw std::overflow_error(
                "Upload body exceeds the supported file size."
            );
        }

        std::ofstream output{
            file_path_,
            std::ios::binary
                | std::ios::trunc
        };

        if (!output.is_open())
        {
            throw std::runtime_error(
                "Failed to create the temporary upload file."
            );
        }

        if (!body.empty())
        {
            output.write(
                body.data(),
                static_cast<std::streamsize>(
                    body.size()
                )
            );
        }

        output.flush();

        if (!output)
        {
            throw std::runtime_error(
                "Failed while writing the temporary upload file."
            );
        }
    }

    [[nodiscard]] const std::filesystem::path&
    path() const noexcept
    {
        return file_path_;
    }

private:
    static std::filesystem::path
    create_unique_directory()
    {
        std::error_code temp_error;

        const std::filesystem::path temp_root =
            std::filesystem::temp_directory_path(
                temp_error
            );

        if (temp_error)
        {
            throw std::runtime_error(
                "Failed to locate the temporary directory: "
                + temp_error.message()
            );
        }

        const auto timestamp =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();

        for (
            std::size_t attempt = 0;
            attempt < 128;
            ++attempt
        )
        {
            const std::uint64_t sequence =
                upload_sequence.fetch_add(
                    1,
                    std::memory_order_relaxed
                );

            const std::filesystem::path candidate =
                temp_root
                / (
                    "nexusfs-upload-"
                    + std::to_string(timestamp)
                    + "-"
                    + std::to_string(sequence)
                    + "-"
                    + std::to_string(attempt)
                );

            std::error_code create_error;

            const bool created =
                std::filesystem::create_directory(
                    candidate,
                    create_error
                );

            if (created)
            {
                return candidate;
            }

            if (
                create_error &&
                create_error !=
                    std::errc::file_exists
            )
            {
                throw std::runtime_error(
                    "Failed to create a temporary upload directory: "
                    + create_error.message()
                );
            }
        }

        throw std::runtime_error(
            "Failed to allocate a unique temporary upload directory."
        );
    }

    void cleanup() noexcept
    {
        if (directory_.empty())
        {
            return;
        }

        std::error_code cleanup_error;

        std::filesystem::remove_all(
            directory_,
            cleanup_error
        );
    }

    std::filesystem::path directory_;
    std::filesystem::path file_path_;
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

bool is_ascii_whitespace(
    char character
)
{
    return (
        character == ' ' ||
        character == '\t' ||
        character == '\r' ||
        character == '\n'
    );
}

std::string_view trim_ascii_whitespace(
    std::string_view value
)
{
    while (
        !value.empty() &&
        is_ascii_whitespace(
            value.front()
        )
    )
    {
        value.remove_prefix(1);
    }

    while (
        !value.empty() &&
        is_ascii_whitespace(
            value.back()
        )
    )
    {
        value.remove_suffix(1);
    }

    return value;
}

bool has_binary_content_type(
    const HttpRouter::Request& request
)
{
    const auto field_value =
        request[
            beast_http::field::content_type
        ];

    if (field_value.empty())
    {
        return false;
    }

    std::string_view media_type{
        field_value.data(),
        field_value.size()
    };

    const std::size_t parameter_position =
        media_type.find(';');

    if (
        parameter_position !=
        std::string_view::npos
    )
    {
        media_type =
            media_type.substr(
                0,
                parameter_position
            );
    }

    media_type =
        trim_ascii_whitespace(
            media_type
        );

    return beast::iequals(
        beast::string_view{
            media_type.data(),
            media_type.size()
        },
        beast::string_view{
            binary_content_type.data(),
            binary_content_type.size()
        }
    );
}

std::optional<std::string>
read_upload_filename(
    const HttpRouter::Request& request
)
{
    const auto iterator =
        request.find(
            beast::string_view{
                upload_filename_header.data(),
                upload_filename_header.size()
            }
        );

    if (iterator == request.end())
    {
        return std::nullopt;
    }

    const auto value =
        iterator->value();

    return std::string{
        value.data(),
        value.size()
    };
}

std::string uppercase_ascii(
    std::string_view value
)
{
    std::string result;
    result.reserve(
        value.size()
    );

    for (char character : value)
    {
        if (
            character >= 'a' &&
            character <= 'z'
        )
        {
            result.push_back(
                static_cast<char>(
                    character
                    - 'a'
                    + 'A'
                )
            );
        }
        else
        {
            result.push_back(
                character
            );
        }
    }

    return result;
}

bool is_reserved_windows_filename(
    std::string_view filename
)
{
    const std::size_t extension_position =
        filename.find('.');

    const std::string_view stem =
        filename.substr(
            0,
            extension_position
        );

    const std::string uppercase_stem =
        uppercase_ascii(
            stem
        );

    constexpr std::array<
        std::string_view,
        5
    > reserved_names{
        "CON",
        "PRN",
        "AUX",
        "NUL",
        "CLOCK$"
    };

    if (
        std::find(
            reserved_names.begin(),
            reserved_names.end(),
            uppercase_stem
        ) != reserved_names.end()
    )
    {
        return true;
    }

    if (uppercase_stem.size() == 4)
    {
        const bool reserved_prefix =
            uppercase_stem.starts_with(
                "COM"
            ) ||
            uppercase_stem.starts_with(
                "LPT"
            );

        const char suffix =
            uppercase_stem[3];

        if (
            reserved_prefix &&
            suffix >= '1' &&
            suffix <= '9'
        )
        {
            return true;
        }
    }

    return false;
}

bool is_valid_upload_filename(
    std::string_view filename
)
{
    if (
        filename.empty() ||
        filename.size() >
            maximum_filename_length
    )
    {
        return false;
    }

    if (
        filename == "." ||
        filename == ".."
    )
    {
        return false;
    }

    if (
        filename.front() == ' ' ||
        filename.back() == ' ' ||
        filename.back() == '.'
    )
    {
        return false;
    }

    constexpr std::string_view
        invalid_characters{
            "<>:\"/\\|?*"
        };

    for (char character : filename)
    {
        const auto byte =
            static_cast<unsigned char>(
                character
            );

        if (
            byte < 0x20U ||
            byte > 0x7EU
        )
        {
            return false;
        }

        if (
            invalid_characters.find(
                character
            ) != std::string_view::npos
        )
        {
            return false;
        }
    }

    return !is_reserved_windows_filename(
        filename
    );
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
    std::string allowed_methods
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
        std::move(allowed_methods)
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

HttpRouter::Response make_invalid_upload_filename_response(
    const HttpRouter::Request& request
)
{
    return make_error_response(
        request,
        beast_http::status::bad_request,
        "invalid_upload_filename",
        "X-NexusFS-Filename must contain a safe "
        "portable filename without a directory path."
    );
}

HttpRouter::Response make_unsupported_media_type_response(
    const HttpRouter::Request& request
)
{
    return make_error_response(
        request,
        beast_http::status::unsupported_media_type,
        "unsupported_media_type",
        "File uploads require the "
        "application/octet-stream content type."
    );
}

HttpRouter::Response make_upload_failed_response(
    const HttpRouter::Request& request
)
{
    return make_error_response(
        request,
        beast_http::status::internal_server_error,
        "upload_failed",
        "The uploaded file could not be stored."
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

HttpRouter::Response
make_invalid_request_body_response(
    const HttpRouter::Request& request
)
{
    return make_error_response(
        request,
        beast_http::status::bad_request,
        "invalid_request_body",
        "The request body must be a JSON object "
        "containing a non-empty output_path string."
    );
}

HttpRouter::Response
make_output_path_exists_response(
    const HttpRouter::Request& request
)
{
    return make_error_response(
        request,
        beast_http::status::conflict,
        "output_path_exists",
        "The requested restoration output path "
        "already exists."
    );
}

HttpRouter::Response
make_restoration_failed_response(
    const HttpRouter::Request& request
)
{
    return make_error_response(
        request,
        beast_http::status::conflict,
        "restoration_failed",
        "The file could not be restored from "
        "the currently stored chunks."
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

HttpRouter::Response make_upload_response(
    const HttpRouter::Request& request,
    const app::StoreFileResult& result
)
{
    const bool newly_created =
        result.manifest_stored;

    const nlohmann::ordered_json payload = {
        {
            "stored_file",
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
                    "chunks_stored",
                    result.chunks_stored
                },
                {
                    "chunks_reused",
                    result.chunks_reused
                },
                {
                    "bytes_processed",
                    result.bytes_processed
                },
                {
                    "encoded_manifest_size",
                    result.encoded_manifest_size
                },
                {
                    "manifest_stored",
                    result.manifest_stored
                },
                {
                    "status",
                    newly_created
                        ? "stored"
                        : "reused"
                }
            }
        }
    };

    return make_json_response(
        newly_created
            ? beast_http::status::created
            : beast_http::status::ok,
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

HttpRouter::Response make_restoration_response(
    const HttpRouter::Request& request,
    const app::RestoreFileResult& result
)
{
    const nlohmann::ordered_json payload = {
        {
            "restoration",
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
                    "output_path",
                    result.output_path.string()
                },
                {
                    "bytes_written",
                    result.bytes_written
                },
                {
                    "chunks_loaded",
                    result.chunks_loaded
                },
                {
                    "status",
                    "restored"
                }
            }
        }
    };

    return make_json_response(
        beast_http::status::created,
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

HttpRouter::Response handle_upload(
    const HttpRouter::Request& request,
    const app::NexusFsService& service
)
{
    if (!has_binary_content_type(request))
    {
        return make_unsupported_media_type_response(
            request
        );
    }

    const std::optional<std::string> filename =
        read_upload_filename(
            request
        );

    if (
        !filename.has_value() ||
        !is_valid_upload_filename(
            *filename
        )
    )
    {
        return make_invalid_upload_filename_response(
            request
        );
    }

    try
    {
        const TemporaryUploadFile temporary_file{
            *filename
        };

        temporary_file.write(
            request.body()
        );

        return make_upload_response(
            request,
            service.store_file(
                temporary_file.path()
            )
        );
    }
    catch (const std::exception&)
    {
        return make_upload_failed_response(
            request
        );
    }
}

HttpRouter::Response handle_restoration(
    const HttpRouter::Request& request,
    const app::NexusFsService& service,
    const std::string& manifest_id
)
{
    const nlohmann::json request_body =
        nlohmann::json::parse(
            request.body(),
            nullptr,
            false
        );

    if (
        request_body.is_discarded() ||
        !request_body.is_object() ||
        !request_body.contains(
            "output_path"
        ) ||
        !request_body.at(
            "output_path"
        ).is_string()
    )
    {
        return make_invalid_request_body_response(
            request
        );
    }

    const std::string output_path_text =
        request_body.at(
            "output_path"
        ).get<std::string>();

    if (output_path_text.empty())
    {
        return make_invalid_request_body_response(
            request
        );
    }

    const std::filesystem::path output_path{
        output_path_text
    };

    std::error_code exists_error;

    const bool output_exists =
        std::filesystem::exists(
            output_path,
            exists_error
        );

    if (exists_error)
    {
        return make_internal_error_response(
            request
        );
    }

    if (output_exists)
    {
        return make_output_path_exists_response(
            request
        );
    }

    try
    {
        return make_restoration_response(
            request,
            service.restore_file(
                manifest_id,
                output_path
            )
        );
    }
    catch (const std::exception&)
    {
        return make_restoration_failed_response(
            request
        );
    }
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

HttpRouter::Response HttpRouter::route_application(
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
            request.method() ==
            beast_http::verb::get
        )
        {
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

        if (
            request.method() ==
            beast_http::verb::post
        )
        {
            return handle_upload(
                request,
                *service_
            );
        }

        return make_method_not_allowed_response(
            request,
            "GET, POST"
        );
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

        const bool is_restore_route =
            remaining_target.ends_with(
                restore_route_suffix
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
        else if (is_restore_route)
        {
            remaining_target.remove_suffix(
                restore_route_suffix.size()
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
                remaining_target.find('/') !=
                std::string_view::npos
            )
            {
                return make_not_found_response(
                    request
                );
            }

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
            remaining_target.find('/') !=
            std::string_view::npos
        )
        {
            return make_not_found_response(
                request
            );
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

            if (is_restore_route)
            {
                return handle_restoration(
                    request,
                    *service_,
                    manifest_id
                );
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