#include "nexusfs/http/http_server.hpp"

#include "nexusfs/http/http_session.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/beast/core/error.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nexusfs::http
{

namespace asio = boost::asio;
namespace beast = boost::beast;

using Tcp = asio::ip::tcp;

namespace
{

constexpr std::chrono::milliseconds
    accept_retry_delay{
        10
    };

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

bool is_accept_retry_error(
    const beast::error_code& error
)
{
    return (
        error == asio::error::would_block ||
        error == asio::error::try_again
    );
}

bool is_shutdown_accept_error(
    const beast::error_code& error
)
{
    return (
        error == asio::error::operation_aborted ||
        error == asio::error::bad_descriptor ||
        error == asio::error::not_connected
    );
}

void close_socket(
    Tcp::socket& socket
) noexcept
{
    beast::error_code ignored_error;

    socket.cancel(
        ignored_error
    );

    socket.shutdown(
        Tcp::socket::shutdown_both,
        ignored_error
    );

    socket.close(
        ignored_error
    );
}

}

struct HttpServer::State
{
    struct Worker
    {
        std::shared_ptr<HttpSession> session;

        std::shared_ptr<
            std::atomic<bool>
        > finished;

        std::thread thread;
    };

    std::atomic<bool> run_started{
        false
    };

    std::atomic<bool> running{
        false
    };

    std::atomic<bool> stop_requested{
        false
    };

    std::atomic<std::uint16_t> bound_port{
        0
    };

    std::atomic<std::size_t> active_sessions{
        0
    };

    mutable std::mutex workers_mutex;

    std::vector<
        std::unique_ptr<Worker>
    > workers;

    void stop_sessions() noexcept
    {
        std::vector<
            std::shared_ptr<HttpSession>
        > sessions;

        try
        {
            {
                const std::lock_guard lock{
                    workers_mutex
                };

                sessions.reserve(
                    workers.size()
                );

                for (
                    const auto& worker :
                    workers
                )
                {
                    if (
                        worker &&
                        worker->session
                    )
                    {
                        sessions.push_back(
                            worker->session
                        );
                    }
                }
            }

            for (
                const auto& session :
                sessions
            )
            {
                session->stop();
            }
        }
        catch (...)
        {
            /*
             * stop() is required to remain noexcept.
             * Each session will also close itself during
             * destruction if copying the registry fails.
             */
        }
    }

    void reap_finished_workers()
    {
        std::vector<std::thread>
            completed_threads;

        {
            const std::lock_guard lock{
                workers_mutex
            };

            auto iterator =
                workers.begin();

            while (
                iterator != workers.end()
            )
            {
                const bool finished =
                    (*iterator)->finished->load(
                        std::memory_order_acquire
                    );

                if (!finished)
                {
                    ++iterator;
                    continue;
                }

                if (
                    (*iterator)->thread.joinable()
                )
                {
                    completed_threads.push_back(
                        std::move(
                            (*iterator)->thread
                        )
                    );
                }

                iterator =
                    workers.erase(
                        iterator
                    );
            }
        }

        for (
            std::thread& thread :
            completed_threads
        )
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
    }

    void join_all_workers() noexcept
    {
        std::vector<std::thread>
            remaining_threads;

        try
        {
            {
                const std::lock_guard lock{
                    workers_mutex
                };

                remaining_threads.reserve(
                    workers.size()
                );

                for (
                    auto& worker :
                    workers
                )
                {
                    if (
                        worker &&
                        worker->thread.joinable()
                    )
                    {
                        remaining_threads.push_back(
                            std::move(
                                worker->thread
                            )
                        );
                    }
                }

                workers.clear();
            }

            for (
                std::thread& thread :
                remaining_threads
            )
            {
                if (thread.joinable())
                {
                    thread.join();
                }
            }
        }
        catch (...)
        {
            /*
             * This path should not occur during normal
             * operation. Shutdown methods must not throw.
             */
        }
    }
};

HttpServer::HttpServer(
    std::string address,
    std::uint16_t port,
    HttpRouter router
)
    : address_{std::move(address)},
      requested_port_{port},
      router_{std::move(router)},
      state_{std::make_unique<State>()}
{
    if (address_.empty())
    {
        throw std::invalid_argument(
            "HTTP server address cannot be empty."
        );
    }
}

HttpServer::~HttpServer()
{
    stop();
}

void HttpServer::run()
{
    const bool already_started =
        state_->run_started.exchange(
            true,
            std::memory_order_acq_rel
        );

    if (already_started)
    {
        throw std::logic_error(
            "HTTP server run() may only be called once."
        );
    }

    /*
     * stop() may legally be called before run().
     * In that case the one-shot server remains stopped.
     */
    if (
        state_->stop_requested.load(
            std::memory_order_acquire
        )
    )
    {
        return;
    }

    asio::io_context io_context{1};

    beast::error_code address_error;

    const asio::ip::address parsed_address =
        asio::ip::make_address(
            address_,
            address_error
        );

    if (address_error)
    {
        throw std::runtime_error(
            "Failed to parse the HTTP server address: "
            + address_error.message()
        );
    }

    const Tcp::endpoint endpoint{
        parsed_address,
        requested_port_
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

    /*
     * A non-blocking listener lets stop() use an atomic
     * shutdown request without concurrently manipulating
     * the acceptor from another thread.
     */
    acceptor.non_blocking(
        true,
        error
    );

    if (error)
    {
        throw std::runtime_error(
            "Failed to configure the HTTP acceptor "
            "for graceful shutdown: "
            + error.message()
        );
    }

    const Tcp::endpoint local_endpoint =
        acceptor.local_endpoint(
            error
        );

    if (error)
    {
        throw std::runtime_error(
            "Failed to read the HTTP server endpoint: "
            + error.message()
        );
    }

    state_->bound_port.store(
        local_endpoint.port(),
        std::memory_order_release
    );

    state_->running.store(
        true,
        std::memory_order_release
    );

    std::cout
        << "NexusFS HTTP server listening on http://"
        << address_
        << ':'
        << local_endpoint.port()
        << '\n';

    const auto cleanup =
        [&]() noexcept
        {
            state_->stop_requested.store(
                true,
                std::memory_order_release
            );

            beast::error_code ignored_error;

            acceptor.cancel(
                ignored_error
            );

            acceptor.close(
                ignored_error
            );

            state_->stop_sessions();
            state_->join_all_workers();

            state_->running.store(
                false,
                std::memory_order_release
            );
        };

    try
    {
        while (
            !state_->stop_requested.load(
                std::memory_order_acquire
            )
        )
        {
            state_->reap_finished_workers();

            Tcp::socket socket{
                io_context
            };

            acceptor.accept(
                socket,
                error
            );

            if (error)
            {
                if (
                    is_accept_retry_error(
                        error
                    )
                )
                {
                    std::this_thread::sleep_for(
                        accept_retry_delay
                    );

                    continue;
                }

                if (
                    state_->stop_requested.load(
                        std::memory_order_acquire
                    ) &&
                    is_shutdown_accept_error(
                        error
                    )
                )
                {
                    break;
                }

                report_network_error(
                    error,
                    "connection accept"
                );

                std::this_thread::sleep_for(
                    accept_retry_delay
                );

                continue;
            }

            if (
                state_->stop_requested.load(
                    std::memory_order_acquire
                )
            )
            {
                close_socket(
                    socket
                );

                break;
            }

            const auto session =
                std::make_shared<HttpSession>(
                    std::move(socket),
                    router_
                );

            const auto finished =
                std::make_shared<
                    std::atomic<bool>
                >(
                    false
                );

            auto worker =
                std::make_unique<
                    State::Worker
                >();

            worker->session =
                session;

            worker->finished =
                finished;

            state_->active_sessions.fetch_add(
                1,
                std::memory_order_acq_rel
            );

            try
            {
                worker->thread =
                    std::thread{
                        [
                            session,
                            finished,
                            state = state_.get()
                        ]()
                        {
                            try
                            {
                                session->run();
                            }
                            catch (
                                const std::exception&
                                    exception
                            )
                            {
                                if (
                                    !session->
                                        stop_requested()
                                )
                                {
                                    std::cerr
                                        << "NexusFS HTTP "
                                        << "session error: "
                                        << exception.what()
                                        << '\n';
                                }
                            }
                            catch (...)
                            {
                                if (
                                    !session->
                                        stop_requested()
                                )
                                {
                                    std::cerr
                                        << "NexusFS HTTP "
                                        << "session error: "
                                        << "unknown exception\n";
                                }
                            }

                            finished->store(
                                true,
                                std::memory_order_release
                            );

                            state->
                                active_sessions
                                .fetch_sub(
                                    1,
                                    std::memory_order_acq_rel
                                );
                        }
                    };
            }
            catch (...)
            {
                state_->active_sessions.fetch_sub(
                    1,
                    std::memory_order_acq_rel
                );

                session->stop();

                throw;
            }

            try
            {
                const std::lock_guard lock{
                    state_->workers_mutex
                };

                state_->workers.push_back(
                    std::move(worker)
                );
            }
            catch (...)
            {
                session->stop();

                if (
                    worker &&
                    worker->thread.joinable()
                )
                {
                    worker->thread.join();
                }

                throw;
            }

            /*
             * This closes the race where stop() occurs after
             * the accept but immediately before registration.
             */
            if (
                state_->stop_requested.load(
                    std::memory_order_acquire
                )
            )
            {
                session->stop();
            }
        }

        cleanup();
    }
    catch (...)
    {
        cleanup();
        throw;
    }
}

void HttpServer::stop() noexcept
{
    state_->stop_requested.store(
        true,
        std::memory_order_release
    );

    state_->stop_sessions();
}

bool HttpServer::is_running() const noexcept
{
    return state_->running.load(
        std::memory_order_acquire
    );
}

bool HttpServer::stop_requested() const noexcept
{
    return state_->stop_requested.load(
        std::memory_order_acquire
    );
}

std::size_t
HttpServer::active_session_count() const noexcept
{
    return state_->active_sessions.load(
        std::memory_order_acquire
    );
}

const std::string&
HttpServer::address() const noexcept
{
    return address_;
}

std::uint16_t
HttpServer::port() const noexcept
{
    const std::uint16_t bound_port =
        state_->bound_port.load(
            std::memory_order_acquire
        );

    if (bound_port != 0)
    {
        return bound_port;
    }

    return requested_port_;
}

}