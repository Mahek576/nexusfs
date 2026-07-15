#include "nexusfs/http/http_server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace nexusfs::http
{

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace beast_http = boost::beast::http;

using Tcp = asio::ip::tcp;

namespace
{

constexpr std::uint64_t maximum_request_body_size =
    1024ULL * 1024ULL;

void report_network_error(
    const beast::error_code& error,
    const char* operation
)
{
    std::cerr
        << "NexusFS HTTP "
        << operation
        << " error: "
        << error.message()
        << '\n';
}

void run_http_session(
    Tcp::socket socket,
    HttpRouter router
)
{
    beast::flat_buffer buffer;
    beast::error_code error;

    for (;;)
    {
        beast_http::request_parser<
            beast_http::string_body
        > parser;

        parser.body_limit(
            maximum_request_body_size
        );

        beast_http::read(
            socket,
            buffer,
            parser,
            error
        );

        if (
            error ==
            beast_http::error::end_of_stream
        )
        {
            break;
        }

        if (error)
        {
            report_network_error(
                error,
                "request read"
            );

            break;
        }

        HttpRouter::Request request =
            parser.release();

        HttpRouter::Response response =
            router.route(request);

        const bool close_connection =
            response.need_eof();

        beast_http::write(
            socket,
            response,
            error
        );

        if (error)
        {
            report_network_error(
                error,
                "response write"
            );

            break;
        }

        if (close_connection)
        {
            break;
        }
    }

    socket.shutdown(
        Tcp::socket::shutdown_send,
        error
    );

    if (
        error &&
        error != asio::error::not_connected
    )
    {
        report_network_error(
            error,
            "connection shutdown"
        );
    }
}

}

HttpServer::HttpServer(
    std::string address,
    std::uint16_t port,
    HttpRouter router
)
    : address_{std::move(address)},
      port_{port},
      router_{std::move(router)}
{
    if (address_.empty())
    {
        throw std::invalid_argument(
            "HTTP server address cannot be empty."
        );
    }

    if (port_ == 0)
    {
        throw std::invalid_argument(
            "HTTP server port must be greater than zero."
        );
    }
}

void HttpServer::run() const
{
    asio::io_context io_context{1};

    const asio::ip::address parsed_address =
        asio::ip::make_address(
            address_
        );

    const Tcp::endpoint endpoint{
        parsed_address,
        port_
    };

    Tcp::acceptor acceptor{
        io_context
    };

    beast::error_code error;

    acceptor.open(
        endpoint.protocol(),
        error
    );

    if (error)
    {
        throw std::runtime_error(
            "Failed to open the HTTP acceptor: "
            + error.message()
        );
    }

    acceptor.set_option(
        asio::socket_base::reuse_address{
            true
        },
        error
    );

    if (error)
    {
        throw std::runtime_error(
            "Failed to configure the HTTP acceptor: "
            + error.message()
        );
    }

    acceptor.bind(
        endpoint,
        error
    );

    if (error)
    {
        throw std::runtime_error(
            "Failed to bind the HTTP server: "
            + error.message()
        );
    }

    acceptor.listen(
        asio::socket_base::
            max_listen_connections,
        error
    );

    if (error)
    {
        throw std::runtime_error(
            "Failed to start HTTP listening: "
            + error.message()
        );
    }

    std::cout
        << "NexusFS HTTP server listening on http://"
        << address_
        << ':'
        << port_
        << '\n';

    for (;;)
    {
        Tcp::socket socket{
            io_context
        };

        acceptor.accept(
            socket,
            error
        );

        if (error)
        {
            report_network_error(
                error,
                "connection accept"
            );

            continue;
        }

        std::thread{
            [
                socket = std::move(socket),
                router = router_
            ]() mutable
            {
                try
                {
                    run_http_session(
                        std::move(socket),
                        std::move(router)
                    );
                }
                catch (
                    const std::exception& exception
                )
                {
                    std::cerr
                        << "NexusFS HTTP session error: "
                        << exception.what()
                        << '\n';
                }
            }
        }.detach();
    }
}

const std::string&
HttpServer::address() const noexcept
{
    return address_;
}

std::uint16_t
HttpServer::port() const noexcept
{
    return port_;
}

}