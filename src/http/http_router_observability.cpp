#include "nexusfs/http/http_router.hpp"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace nexusfs::http
{

namespace beast_http =
    boost::beast::http;

namespace
{

constexpr std::string_view health_route{
    "/api/v1/health"
};

constexpr std::string_view metrics_route{
    "/api/v1/metrics"
};

constexpr std::string_view cluster_route{
    "/api/v1/cluster"
};

constexpr std::string_view cluster_heartbeat_route{
    "/api/v1/cluster/heartbeat"
};

constexpr std::string_view cluster_chunk_prefix{
    "/api/v1/cluster/chunks/"
};

constexpr std::string_view normalized_cluster_chunk_route{
    "/api/v1/cluster/chunks/{chunk_hash}"
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

constexpr std::string_view normalized_file_route{
    "/api/v1/files/{manifest_id}"
};

constexpr std::string_view
    normalized_verification_route{
        "/api/v1/files/{manifest_id}/verify"
    };

constexpr std::string_view
    normalized_restoration_route{
        "/api/v1/files/{manifest_id}/restore"
    };

constexpr std::string_view unmatched_route{
    "/unmatched"
};

std::string_view request_target(
    const HttpRouter::Request& request
) noexcept
{
    const auto target =
        request.target();

    return std::string_view{
        target.data(),
        target.size()
    };
}

bool is_lowercase_sha256_identifier(
    std::string_view value
) noexcept
{
    constexpr std::size_t sha256_hex_length =
        64;

    if (
        value.size() !=
        sha256_hex_length
    )
    {
        return false;
    }

    for (
        const char character :
        value
    )
    {
        const bool is_decimal_digit =
            character >= '0'
            && character <= '9';

        const bool is_lowercase_hex_digit =
            character >= 'a'
            && character <= 'f';

        if (
            !is_decimal_digit
            && !is_lowercase_hex_digit
        )
        {
            return false;
        }
    }

    return true;
}

std::string_view normalized_dynamic_file_route(
    std::string_view target
) noexcept
{
    if (
        !target.starts_with(
            file_route_prefix
        )
    )
    {
        return unmatched_route;
    }

    std::string_view remaining_target =
        target.substr(
            file_route_prefix.size()
        );

    if (remaining_target.empty())
    {
        return unmatched_route;
    }

    if (
        remaining_target.ends_with(
            verify_route_suffix
        )
    )
    {
        remaining_target.remove_suffix(
            verify_route_suffix.size()
        );

        return is_lowercase_sha256_identifier(
            remaining_target
        )
            ? normalized_verification_route
            : unmatched_route;
    }

    if (
        remaining_target.ends_with(
            restore_route_suffix
        )
    )
    {
        remaining_target.remove_suffix(
            restore_route_suffix.size()
        );

        return is_lowercase_sha256_identifier(
            remaining_target
        )
            ? normalized_restoration_route
            : unmatched_route;
    }

    return is_lowercase_sha256_identifier(
        remaining_target
    )
        ? normalized_file_route
        : unmatched_route;
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

HttpRouter::Response make_metrics_method_response(
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
                    "The metrics endpoint supports "
                    "only the GET method."
                }
            }
        }
    };

    HttpRouter::Response response =
        make_json_response(
            beast_http::status::
                method_not_allowed,
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

HttpRouter::Response make_metrics_error_response(
    const HttpRouter::Request& request
)
{
    const nlohmann::ordered_json payload = {
        {
            "error",
            {
                {
                    "code",
                    "metrics_snapshot_failed"
                },
                {
                    "message",
                    "The operational metrics snapshot "
                    "could not be generated."
                }
            }
        }
    };

    return make_json_response(
        beast_http::status::
            internal_server_error,
        payload,
        request.version(),
        request.keep_alive()
    );
}

nlohmann::ordered_json make_method_counters(
    const observability::MetricsSnapshot&
        snapshot
)
{
    nlohmann::ordered_json counters =
        nlohmann::ordered_json::array();

    for (
        const observability::NamedCounter&
            counter :
        snapshot.requests_by_method
    )
    {
        counters.push_back(
            {
                {
                    "method",
                    counter.name
                },
                {
                    "count",
                    counter.count
                }
            }
        );
    }

    return counters;
}

nlohmann::ordered_json make_route_counters(
    const observability::MetricsSnapshot&
        snapshot
)
{
    nlohmann::ordered_json counters =
        nlohmann::ordered_json::array();

    for (
        const observability::HttpRouteCounter&
            counter :
        snapshot.requests_by_route
    )
    {
        counters.push_back(
            {
                {
                    "method",
                    counter.method
                },
                {
                    "route",
                    counter.route
                },
                {
                    "count",
                    counter.count
                }
            }
        );
    }

    return counters;
}

nlohmann::ordered_json make_status_counters(
    const observability::MetricsSnapshot&
        snapshot
)
{
    nlohmann::ordered_json counters =
        nlohmann::ordered_json::array();

    for (
        const observability::HttpStatusCounter&
            counter :
        snapshot.responses_by_status
    )
    {
        counters.push_back(
            {
                {
                    "status_code",
                    counter.status_code
                },
                {
                    "count",
                    counter.count
                }
            }
        );
    }

    return counters;
}

HttpRouter::Response make_metrics_response(
    const HttpRouter::Request& request,
    const observability::MetricsSnapshot&
        snapshot
)
{
    const std::uint64_t completed_requests =
        snapshot.requests_succeeded
        + snapshot.requests_failed;

    const std::uint64_t
        average_latency_microseconds =
            completed_requests == 0
            ? 0
            : (
                snapshot
                    .request_latency_microseconds_total
                / completed_requests
            );

    const nlohmann::ordered_json payload = {
        {
            "service",
            "nexusfs"
        },
        {
            "api_version",
            "v1"
        },
        {
            "scope",
            "process_local"
        },
        {
            "uptime_milliseconds",
            snapshot.uptime_milliseconds
        },
        {
            "connections",
            {
                {
                    "total",
                    snapshot.connections_total
                },
                {
                    "active",
                    snapshot.connections_active
                },
                {
                    "peak",
                    snapshot.connections_peak
                }
            }
        },
        {
            "http",
            {
                {
                    "requests",
                    {
                        {
                            "total",
                            snapshot.requests_total
                        },
                        {
                            "in_flight",
                            snapshot.requests_in_flight
                        },
                        {
                            "succeeded",
                            snapshot.requests_succeeded
                        },
                        {
                            "failed",
                            snapshot.requests_failed
                        }
                    }
                },
                {
                    "latency_microseconds",
                    {
                        {
                            "total",
                            snapshot
                                .request_latency_microseconds_total
                        },
                        {
                            "maximum",
                            snapshot
                                .request_latency_microseconds_max
                        },
                        {
                            "average",
                            average_latency_microseconds
                        }
                    }
                },
                {
                    "requests_by_method",
                    make_method_counters(
                        snapshot
                    )
                },
                {
                    "requests_by_route",
                    make_route_counters(
                        snapshot
                    )
                },
                {
                    "responses_by_status",
                    make_status_counters(
                        snapshot
                    )
                }
            }
        },
        {
            "operations",
            {
                {
                    "uploads",
                    {
                        {
                            "total",
                            snapshot.uploads_total
                        },
                        {
                            "succeeded",
                            snapshot.uploads_succeeded
                        },
                        {
                            "failed",
                            snapshot.uploads_failed
                        },
                        {
                            "bytes_processed",
                            snapshot.upload_bytes_processed
                        }
                    }
                },
                {
                    "restorations",
                    {
                        {
                            "total",
                            snapshot.restorations_total
                        },
                        {
                            "succeeded",
                            snapshot.restorations_succeeded
                        },
                        {
                            "failed",
                            snapshot.restorations_failed
                        },
                        {
                            "bytes_written",
                            snapshot.restoration_bytes_written
                        }
                    }
                },
                {
                    "verifications",
                    {
                        {
                            "total",
                            snapshot.verifications_total
                        },
                        {
                            "succeeded",
                            snapshot.verifications_succeeded
                        },
                        {
                            "failed",
                            snapshot.verifications_failed
                        },
                        {
                            "bytes_processed",
                            snapshot.verification_bytes_processed
                        }
                    }
                }
            }
        },
        {
            "storage",
            {
                {
                    "stored_manifests",
                    snapshot.storage_manifests
                },
                {
                    "complete_manifests",
                    snapshot
                        .storage_complete_manifests
                },
                {
                    "incomplete_manifests",
                    snapshot
                        .storage_incomplete_manifests
                },
                {
                    "chunk_references",
                    snapshot.storage_chunks
                },
                {
                    "missing_chunk_references",
                    snapshot.storage_missing_chunks
                }
            }
        },
        {
            "recovery",
            {
                {
                    "runs",
                    snapshot.recovery_runs_total
                },
                {
                    "entries_scanned",
                    snapshot.recovery_entries_scanned
                },
                {
                    "temporary_entries_found",
                    snapshot
                        .recovery_temporary_entries_found
                },
                {
                    "temporary_files_removed",
                    snapshot
                        .recovery_temporary_files_removed
                },
                {
                    "non_regular_entries_preserved",
                    snapshot
                        .recovery_non_regular_entries_preserved
                }
            }
        },
        {
            "cluster_transport",
            {
                {
                    "heartbeats",
                    {
                        {
                            "attempted",
                            snapshot.heartbeat_attempts_total
                        },
                        {
                            "succeeded",
                            snapshot
                                .heartbeat_attempts_succeeded
                        },
                        {
                            "failed",
                            snapshot.heartbeat_attempts_failed
                        }
                    }
                },
                {
                    "replication",
                    {
                        {
                            "chunks_total",
                            snapshot.replication_chunks_total
                        },
                        {
                            "chunks_satisfied",
                            snapshot
                                .replication_chunks_satisfied
                        },
                        {
                            "chunks_failed",
                            snapshot.replication_chunks_failed
                        },
                        {
                            "remote_acknowledgements",
                            snapshot
                                .replication_remote_acknowledgements
                        },
                        {
                            "remote_failures",
                            snapshot
                                .replication_remote_failures
                        }
                    }
                },
                {
                    "recovery",
                    {
                        {
                            "remote_reads_total",
                            snapshot.remote_chunk_reads_total
                        },
                        {
                            "remote_reads_succeeded",
                            snapshot
                                .remote_chunk_reads_succeeded
                        },
                        {
                            "remote_reads_failed",
                            snapshot.remote_chunk_reads_failed
                        },
                        {
                            "local_repairs",
                            snapshot.local_chunk_repairs_total
                        }
                    }
                },
                {
                    "maintenance",
                    {
                        {
                            "runs",
                            snapshot.replica_maintenance_runs_total
                        },
                        {
                            "chunks_scanned",
                            snapshot
                                .replica_maintenance_chunks_scanned
                        },
                        {
                            "local_chunks_recovered",
                            snapshot
                                .replica_maintenance_local_chunks_recovered
                        },
                        {
                            "remote_replicas_observed",
                            snapshot
                                .replica_maintenance_remote_replicas_observed
                        },
                        {
                            "remote_replicas_created",
                            snapshot
                                .replica_maintenance_remote_replicas_created
                        },
                        {
                            "peer_failures",
                            snapshot
                                .replica_maintenance_peer_failures
                        },
                        {
                            "under_replicated_chunks",
                            snapshot
                                .replica_maintenance_under_replicated_chunks
                        },
                        {
                            "scheduler",
                            {
                                {
                                    "starts",
                                    snapshot
                                        .replica_maintenance_scheduler_starts_total
                                },
                                {
                                    "stops",
                                    snapshot
                                        .replica_maintenance_scheduler_stops_total
                                },
                                {
                                    "sweep_failures",
                                    snapshot
                                        .replica_maintenance_scheduler_failures_total
                                }
                            }
                        }
                    }
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

bool response_is_successful(
    const HttpRouter::Response& response
) noexcept
{
    const unsigned int status_code =
        static_cast<unsigned int>(
            response.result_int()
        );

    return (
        status_code >= 200U
        && status_code < 400U
    );
}

std::uint64_t size_to_uint64(
    std::size_t value
) noexcept
{
    if constexpr (
        sizeof(std::size_t)
        > sizeof(std::uint64_t)
    )
    {
        if (
            value >
            static_cast<std::size_t>(
                std::numeric_limits<
                    std::uint64_t
                >::max()
            )
        )
        {
            return std::numeric_limits<
                std::uint64_t
            >::max();
        }
    }

    return static_cast<std::uint64_t>(
        value
    );
}

std::uint64_t saturating_add(
    std::uint64_t left,
    std::uint64_t right
) noexcept
{
    const std::uint64_t maximum =
        std::numeric_limits<
            std::uint64_t
        >::max();

    if (right > maximum - left)
    {
        return maximum;
    }

    return left + right;
}

std::optional<std::uint64_t>
json_unsigned_value(
    const nlohmann::json& value
) noexcept
{
    try
    {
        if (value.is_number_unsigned())
        {
            return value.get<
                std::uint64_t
            >();
        }

        if (value.is_number_integer())
        {
            const std::int64_t signed_value =
                value.get<
                    std::int64_t
                >();

            if (signed_value >= 0)
            {
                return static_cast<
                    std::uint64_t
                >(
                    signed_value
                );
            }
        }
    }
    catch (...)
    {
    }

    return std::nullopt;
}

std::optional<std::uint64_t>
read_nested_unsigned(
    const std::string& response_body,
    std::string_view object_name,
    std::string_view field_name
) noexcept
{
    try
    {
        const nlohmann::json payload =
            nlohmann::json::parse(
                response_body,
                nullptr,
                false
            );

        if (
            payload.is_discarded()
            || !payload.is_object()
        )
        {
            return std::nullopt;
        }

        const std::string object_key{
            object_name
        };

        const std::string field_key{
            field_name
        };

        if (
            !payload.contains(
                object_key
            )
            || !payload.at(
                object_key
            ).is_object()
            || !payload.at(
                object_key
            ).contains(
                field_key
            )
        )
        {
            return std::nullopt;
        }

        return json_unsigned_value(
            payload.at(
                object_key
            ).at(
                field_key
            )
        );
    }
    catch (...)
    {
        return std::nullopt;
    }
}

}

HttpRouter::HttpRouter(
    std::shared_ptr<
        const app::NexusFsService
    > service,
    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry
)
    : service_{
          std::move(service)
      },
      metrics_registry_{
          std::move(metrics_registry)
      }
{
    if (!service_)
    {
        throw std::invalid_argument(
            "HTTP router service cannot be null."
        );
    }

    if (!metrics_registry_)
    {
        throw std::invalid_argument(
            "HTTP router metrics registry cannot be null."
        );
    }
}

HttpRouter::Response HttpRouter::route(
    const Request& request
) const
{
    const std::string_view target =
        request_target(
            request
        );

    if (target == metrics_route)
    {
        if (
            request.method()
            != beast_http::verb::get
        )
        {
            return make_metrics_method_response(
                request
            );
        }

        try
        {
            refresh_storage_catalog_metrics();

            return make_metrics_response(
                request,
                metrics_registry_->snapshot()
            );
        }
        catch (...)
        {
            return make_metrics_error_response(
                request
            );
        }
    }

    Response response =
        route_application(
            request
        );

    record_operation_metrics(
        request,
        response
    );

    return response;
}

std::string_view HttpRouter::normalized_route(
    const Request& request
) const noexcept
{
    const std::string_view target =
        request_target(
            request
        );

    if (
        target.find('?')
        != std::string_view::npos
    )
    {
        return unmatched_route;
    }

    if (target == health_route)
    {
        return health_route;
    }

    if (target == metrics_route)
    {
        return metrics_route;
    }

    if (target == cluster_route)
    {
        return cluster_route;
    }

    if (target == cluster_heartbeat_route)
    {
        return cluster_heartbeat_route;
    }

    if (
        target.starts_with(
            cluster_chunk_prefix
        )
    )
    {
        const std::string_view chunk_hash =
            target.substr(
                cluster_chunk_prefix.size()
            );

        return is_lowercase_sha256_identifier(
            chunk_hash
        )
            ? normalized_cluster_chunk_route
            : unmatched_route;
    }

    if (target == files_route)
    {
        return files_route;
    }

    if (
        target.starts_with(
            file_route_prefix
        )
    )
    {
        return normalized_dynamic_file_route(
            target
        );
    }

    return unmatched_route;
}

observability::MetricsRegistry&
HttpRouter::metrics_registry() const noexcept
{
    return *metrics_registry_;
}

void HttpRouter::record_operation_metrics(
    const Request& request,
    const Response& response
) const noexcept
{
    try
    {
        const std::string_view route_label =
            normalized_route(
                request
            );

        const bool succeeded =
            response_is_successful(
                response
            );

        if (
            route_label == files_route
            && request.method()
                == beast_http::verb::post
        )
        {
            metrics_registry_->
                upload_finished(
                    succeeded
                        ? size_to_uint64(
                              request.body().size()
                          )
                        : 0,
                    succeeded
                );

            if (succeeded)
            {
                refresh_storage_catalog_metrics();
            }

            return;
        }

        if (
            route_label
                == normalized_restoration_route
            && request.method()
                == beast_http::verb::post
        )
        {
            const std::uint64_t bytes_written =
                succeeded
                ? read_nested_unsigned(
                      response.body(),
                      "restoration",
                      "bytes_written"
                  ).value_or(0)
                : 0;

            metrics_registry_->
                restoration_finished(
                    bytes_written,
                    succeeded
                );

            return;
        }

        if (
            route_label
                == normalized_verification_route
            && request.method()
                == beast_http::verb::post
        )
        {
            const std::uint64_t bytes_verified =
                succeeded
                ? read_nested_unsigned(
                      response.body(),
                      "verification",
                      "total_bytes_verified"
                  ).value_or(0)
                : 0;

            metrics_registry_->
                verification_finished(
                    bytes_verified,
                    succeeded
                );

            return;
        }

        if (
            route_label == files_route
            && request.method()
                == beast_http::verb::get
            && succeeded
        )
        {
            refresh_storage_catalog_metrics();
        }
    }
    catch (...)
    {
        /*
         * Metrics collection must never alter API behavior.
         */
    }
}

void HttpRouter::
refresh_storage_catalog_metrics()
    const noexcept
{
    try
    {
        const app::ListFilesResult catalog =
            service_->list_files();

        std::uint64_t chunk_references =
            0;

        std::uint64_t
            missing_chunk_references =
                0;

        for (
            const app::StoredFileSummary& file :
            catalog.files
        )
        {
            chunk_references =
                saturating_add(
                    chunk_references,
                    size_to_uint64(
                        file.chunk_count
                    )
                );

            missing_chunk_references =
                saturating_add(
                    missing_chunk_references,
                    size_to_uint64(
                        file.missing_chunks
                    )
                );
        }

        metrics_registry_->
            set_storage_catalog(
                size_to_uint64(
                    catalog.files.size()
                ),
                size_to_uint64(
                    catalog.complete_manifests
                ),
                size_to_uint64(
                    catalog.incomplete_manifests
                ),
                chunk_references,
                missing_chunk_references
            );
    }
    catch (...)
    {
        /*
         * Keep the last valid catalog gauges when storage cannot
         * temporarily be inspected.
         */
    }
}

}