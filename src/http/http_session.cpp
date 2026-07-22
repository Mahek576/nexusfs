#include "nexusfs/http/http_session.hpp"

#include <boost/asio/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace nexusfs::http
{

namespace beast_http =
    boost::beast::http;

namespace
{

constexpr std::uint64_t maximum_request_body_size =
    64ULL * 1024ULL * 1024ULL;

constexpr std::chrono::seconds session_timeout{
    30
};

bool is_shutdown_error(
    const boost::system::error_code& error
)
{
    return (
        error
            == boost::asio::error::
                operation_aborted
        || error
            == boost::asio::error::
                bad_descriptor
        || error
            == boost::asio::error::
                not_connected
        || error
            == boost::asio::error::
                connection_aborted
    );
}

void close_socket(
    boost::beast::tcp_stream& stream
) noexcept
{
    boost::system::error_code ignored_error;

    stream.socket().cancel(
        ignored_error
    );

    stream.socket().shutdown(
        boost::asio::ip::tcp::socket::
            shutdown_both,
        ignored_error
    );

    stream.socket().close(
        ignored_error
    );
}

observability::LogLevel level_for_status(
    unsigned int status_code
) noexcept
{
    if (status_code >= 500U)
    {
        return observability::LogLevel::error;
    }

    if (status_code >= 400U)
    {
        return observability::LogLevel::warning;
    }

    return observability::LogLevel::info;
}

/*
 * Ensures metrics and structured request logging are completed on
 * every exit path after a valid HTTP request has been parsed.
 */
class RequestObservabilityScope final
{
public:
    using Clock =
        std::chrono::steady_clock;

    RequestObservabilityScope(
        observability::MetricsRegistry&
            metrics_registry,
        observability::JsonLogger&
            logger,
        std::string_view method,
        std::string_view normalized_route
    ) noexcept
        : metrics_registry_{
              metrics_registry
          },
          logger_{
              logger
          },
          method_{
              method
          },
          normalized_route_{
              normalized_route
          },
          started_at_{
              Clock::now()
          }
    {
        metrics_registry_.
            request_started();
    }

    RequestObservabilityScope(
        const RequestObservabilityScope&
    ) = delete;

    RequestObservabilityScope& operator=(
        const RequestObservabilityScope&
    ) = delete;

    ~RequestObservabilityScope()
    {
        finish();
    }

    void set_status_code(
        unsigned int status_code
    ) noexcept
    {
        status_code_ =
            status_code;
    }

    void finish() noexcept
    {
        if (finished_)
        {
            return;
        }

        finished_ =
            true;

        const Clock::time_point finished_at =
            Clock::now();

        const auto latency =
            std::chrono::duration_cast<
                std::chrono::microseconds
            >(
                finished_at
                - started_at_
            );

        metrics_registry_.
            request_finished(
                method_,
                normalized_route_,
                status_code_,
                latency
            );

        const std::uint64_t
            latency_microseconds =
                latency.count() > 0
                ? static_cast<std::uint64_t>(
                      latency.count()
                  )
                : 0;

        const bool successful =
            status_code_ >= 200U
            && status_code_ < 400U;

        logger_.log(
            level_for_status(
                status_code_
            ),
            "http_request_completed",
            "HTTP request completed.",
            {
                observability::LogField{
                    "method",
                    std::string{
                        method_
                    }
                },
                observability::LogField{
                    "route",
                    std::string{
                        normalized_route_
                    }
                },
                observability::LogField{
                    "status_code",
                    static_cast<std::uint64_t>(
                        status_code_
                    )
                },
                observability::LogField{
                    "latency_microseconds",
                    latency_microseconds
                },
                observability::LogField{
                    "successful",
                    successful
                }
            }
        );
    }

private:
    observability::MetricsRegistry&
        metrics_registry_;

    observability::JsonLogger&
        logger_;

    std::string_view method_;
    std::string_view normalized_route_;

    Clock::time_point started_at_;

    unsigned int status_code_{
        500
    };

    bool finished_{
        false
    };
};

}

HttpSession::HttpSession(
    boost::asio::ip::tcp::socket socket,
    HttpRouter router
)
    : stream_{
          std::move(socket)
      },
      router_{
          std::move(router)
      }
{
    router_.metrics_registry().
        connection_opened();
}

HttpSession::~HttpSession()
{
    stop();

    close_connection_metric();
}

void HttpSession::run()
{
    boost::beast::flat_buffer buffer;

    try
    {
        while (
            !stop_requested_.load(
                std::memory_order_acquire
            )
        )
        {
            beast_http::request_parser<
                beast_http::string_body
            > parser;

            parser.body_limit(
                maximum_request_body_size
            );

            stream_.expires_after(
                session_timeout
            );

            boost::system::error_code
                read_error;

            beast_http::read(
                stream_,
                buffer,
                parser,
                read_error
            );

            if (
                read_error
                == beast_http::error::
                    end_of_stream
            )
            {
                break;
            }

            if (read_error)
            {
                if (
                    stop_requested_.load(
                        std::memory_order_acquire
                    )
                    && is_shutdown_error(
                        read_error
                    )
                )
                {
                    break;
                }

                throw boost::system::
                    system_error{
                        read_error
                    };
            }

            if (
                stop_requested_.load(
                    std::memory_order_acquire
                )
            )
            {
                break;
            }

            HttpRouter::Request request =
                parser.release();

            const auto method_value =
                request.method_string();

            const std::string_view
                method_label{
                    method_value.data(),
                    method_value.size()
                };

            const std::string_view
                route_label =
                    router_.normalized_route(
                        request
                    );

            RequestObservabilityScope
                request_observability{
                    router_.
                        metrics_registry(),
                    router_.logger(),
                    method_label,
                    route_label
                };

            HttpRouter::Response response =
                router_.route(
                    request
                );

            request_observability.
                set_status_code(
                    static_cast<unsigned int>(
                        response.result_int()
                    )
                );

            const bool should_close =
                response.need_eof();

            stream_.expires_after(
                session_timeout
            );

            boost::system::error_code
                write_error;

            beast_http::write(
                stream_,
                response,
                write_error
            );

            if (write_error)
            {
                if (
                    stop_requested_.load(
                        std::memory_order_acquire
                    )
                    && is_shutdown_error(
                        write_error
                    )
                )
                {
                    break;
                }

                throw boost::system::
                    system_error{
                        write_error
                    };
            }

            /*
             * Include response serialization and socket writing in
             * the recorded end-to-end latency.
             */
            request_observability.finish();

            if (should_close)
            {
                break;
            }
        }
    }
    catch (...)
    {
        close_socket(
            stream_
        );

        close_connection_metric();

        throw;
    }

    close_socket(
        stream_
    );

    close_connection_metric();
}

void HttpSession::stop() noexcept
{
    const bool already_requested =
        stop_requested_.exchange(
            true,
            std::memory_order_acq_rel
        );

    if (already_requested)
    {
        return;
    }

    close_socket(
        stream_
    );
}

bool HttpSession::stop_requested() const noexcept
{
    return stop_requested_.load(
        std::memory_order_acquire
    );
}

void HttpSession::
close_connection_metric() noexcept
{
    const bool already_closed =
        connection_metric_closed_.
            exchange(
                true,
                std::memory_order_acq_rel
            );

    if (already_closed)
    {
        return;
    }

    router_.metrics_registry().
        connection_closed();
}

}