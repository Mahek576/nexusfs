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
     *
     * Each successfully parsed request contributes method, route,
     * response-status and end-to-end latency metrics.
     */
    void run();

    /*
     * Interrupts the connection.
     *
     * This method is noexcept and idempotent. It may be called from
     * the server lifecycle thread while run() executes in a worker.
     */
    void stop() noexcept;

    [[nodiscard]] bool
    stop_requested() const noexcept;

private:
    /*
     * Closes the active-connection metric exactly once.
     *
     * run() normally performs this when the session ends. The
     * destructor provides a fallback for sessions that never run.
     */
    void close_connection_metric() noexcept;

    boost::beast::tcp_stream stream_;
    HttpRouter router_;

    std::atomic<bool> stop_requested_{
        false
    };

    std::atomic<bool>
        connection_metric_closed_{
            false
        };
};

}

#endif