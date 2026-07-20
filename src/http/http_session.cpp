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

}

HttpSession::HttpSession(
    boost::asio::ip::tcp::socket socket,
    HttpRouter router
)
    : stream_{std::move(socket)},
      router_{std::move(router)}
{
}

void HttpSession::run()
{
    boost::beast::flat_buffer buffer;

    for (;;)
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
            throw boost::system::system_error{
                read_error
            };
        }

        HttpRouter::Request request =
            parser.release();

        HttpRouter::Response response =
            router_.route(request);

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
            throw boost::system::system_error{
                write_error
            };
        }

        if (should_close)
        {
            break;
        }
    }

    boost::system::error_code shutdown_error;

    stream_.socket().shutdown(
        boost::asio::ip::tcp::socket::shutdown_send,
        shutdown_error
    );

    if (
        shutdown_error &&
        shutdown_error !=
            boost::asio::error::not_connected
    )
    {
        throw boost::system::system_error{
            shutdown_error
        };
    }
}

}