#ifndef NEXUSFS_HTTP_HTTP_SESSION_HPP
#define NEXUSFS_HTTP_HTTP_SESSION_HPP

#include "nexusfs/http/http_router.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/tcp_stream.hpp>

#include <atomic>

namespace nexusfs::http
{

class HttpSession final
{
public:
    HttpSession(
        boost::asio::ip::tcp::socket socket,
        HttpRouter router
    );

    ~HttpSession();

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

    /*
     * Processes requests on the connection until:
     *
     * - the client closes the connection;
     * - the response requires closure;
     * - a network error occurs; or
     * - stop() requests shutdown.
     */
    void run();

    /*
     * Interrupts the connection.
     *
     * This method is noexcept and idempotent. It may be called
     * from the HTTP server lifecycle thread while run() is
     * executing in the session worker thread.
     */
    void stop() noexcept;

    [[nodiscard]] bool
    stop_requested() const noexcept;

private:
    boost::beast::tcp_stream stream_;
    HttpRouter router_;
    std::atomic<bool> stop_requested_{
        false
    };
};

}

#endif