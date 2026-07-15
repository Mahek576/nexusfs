#ifndef NEXUSFS_HTTP_HTTP_SESSION_HPP
#define NEXUSFS_HTTP_HTTP_SESSION_HPP

#include "nexusfs/http/http_router.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/tcp_stream.hpp>

namespace nexusfs::http
{

class HttpSession final
{
public:
    HttpSession(
        boost::asio::ip::tcp::socket socket,
        HttpRouter router
    );

    HttpSession(
        const HttpSession&
    ) = delete;

    HttpSession& operator=(
        const HttpSession&
    ) = delete;

    HttpSession(
        HttpSession&&
    ) = delete;

    HttpSession& operator=(
        HttpSession&&
    ) = delete;

    void run();

private:
    boost::beast::tcp_stream stream_;
    HttpRouter router_;
};

}

#endif