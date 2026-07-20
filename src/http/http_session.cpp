#include "nexusfs/http/http_session.hpp"

#include <boost/asio/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <chrono>
#include <cstdint>
#include <utility>

namespace nexusfs::http
{

namespace beast_http = boost::beast::http;

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
        error == boost::asio::error::operation_aborted ||
        error == boost::asio::error::bad_descriptor ||
        error == boost::asio::error::not_connected ||
        error == boost::asio::error::connection_aborted
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
        boost::asio::ip::tcp::socket::shutdown_both,
        ignored_error
    );

    stream.socket().close(
        ignored_error
    );
}

}

HttpSession::HttpSession(
    boost::asio::ip::tcp::socket socket,
    HttpRouter router
)
    : stream_{std::move(socket)},
      router_{std::move(router)}
{
}

HttpSession::~HttpSession()
{
    stop();
}

void HttpSession::run()
{
    boost::beast::flat_buffer buffer;

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

        boost::system::error_code read_error;

        beast_http::read(
            stream_,
            buffer,
            parser,
            read_error
        );

        if (
            read_error ==
            beast_http::error::end_of_stream
        )
        {
            break;
        }

        if (read_error)
        {
            if (
                stop_requested_.load(
                    std::memory_order_acquire
                ) &&
                is_shutdown_error(
                    read_error
                )
            )
            {
                break;
            }

            throw boost::system::system_error{
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

        HttpRouter::Response response =
            router_.route(
                request
            );

        const bool should_close =
            response.need_eof();

        stream_.expires_after(
            session_timeout
        );

        boost::system::error_code write_error;

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
                ) &&
                is_shutdown_error(
                    write_error
                )
            )
            {
                break;
            }

            throw boost::system::system_error{
                write_error
            };
        }

        if (should_close)
        {
            break;
        }
    }

    close_socket(
        stream_
    );
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

}