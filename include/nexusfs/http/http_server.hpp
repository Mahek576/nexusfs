#ifndef NEXUSFS_HTTP_HTTP_SERVER_HPP
#define NEXUSFS_HTTP_HTTP_SERVER_HPP

#include "nexusfs/http/http_router.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace nexusfs::http
{

class HttpServer final
{
public:
    HttpServer(
        std::string address,
        std::uint16_t port,
        HttpRouter router
    );

    ~HttpServer();

    HttpServer(
        const HttpServer&
    ) = delete;

    HttpServer& operator=(
        const HttpServer&
    ) = delete;

    HttpServer(
        HttpServer&&
    ) = delete;

    HttpServer& operator=(
        HttpServer&&
    ) = delete;

    /*
     * Starts the blocking accept loop.
     *
     * An HttpServer instance is intentionally one-shot:
     * run() may be called at most once.
     */
    void run();

    /*
     * Requests shutdown.
     *
     * This operation is thread-safe, noexcept and idempotent.
     * It closes the listener and interrupts active sessions.
     */
    void stop() noexcept;

    [[nodiscard]] bool
    is_running() const noexcept;

    [[nodiscard]] bool
    stop_requested() const noexcept;

    [[nodiscard]] std::size_t
    active_session_count() const noexcept;

    [[nodiscard]] const std::string&
    address() const noexcept;

    /*
     * Before binding, this returns the requested port.
     *
     * When the requested port is zero, it returns the
     * operating-system-assigned port after the server starts.
     */
    [[nodiscard]] std::uint16_t
    port() const noexcept;

private:
    struct State;

    std::string address_;
    std::uint16_t requested_port_;
    HttpRouter router_;
    std::unique_ptr<State> state_;
};

}

#endif