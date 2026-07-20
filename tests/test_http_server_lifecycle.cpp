#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace
{

namespace asio = boost::asio;
namespace beast_http = boost::beast::http;

using Tcp = asio::ip::tcp;

constexpr std::chrono::seconds operation_timeout{
    5
};

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        static std::atomic<std::uint64_t> sequence{
            0
        };

        const auto timestamp =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();

        const std::uint64_t current_sequence =
            sequence.fetch_add(
                1,
                std::memory_order_relaxed
            );

        path_ =
            std::filesystem::temp_directory_path()
            / (
                "nexusfs-http-server-tests-"
                + std::to_string(timestamp)
                + "-"
                + std::to_string(current_sequence)
            );

        std::error_code error;

        std::filesystem::create_directories(
            path_,
            error
        );

        if (error)
        {
            throw std::runtime_error(
                "Failed to create HTTP server "
                "test directory: "
                + error.message()
            );
        }
    }

    TemporaryDirectory(
        const TemporaryDirectory&
    ) = delete;

    TemporaryDirectory& operator=(
        const TemporaryDirectory&
    ) = delete;

    ~TemporaryDirectory()
    {
        std::error_code error;

        std::filesystem::remove_all(
            path_,
            error
        );
    }

    [[nodiscard]] const std::filesystem::path&
    path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void require_true(
    bool condition,
    const std::string& test_name
)
{
    if (!condition)
    {
        throw std::runtime_error(
            test_name + " failed."
        );
    }
}

template <typename Actual, typename Expected>
void require_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& test_name
)
{
    if (actual != expected)
    {
        throw std::runtime_error(
            test_name + " failed."
        );
    }
}

template <typename Predicate>
void wait_until(
    Predicate&& predicate,
    const std::string& test_name
)
{
    const auto deadline =
        std::chrono::steady_clock::now()
        + operation_timeout;

    while (
        !std::forward<Predicate>(
            predicate
        )()
    )
    {
        if (
            std::chrono::steady_clock::now()
            >= deadline
        )
        {
            throw std::runtime_error(
                test_name
                + " timed out."
            );
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds{
                5
            }
        );
    }
}

std::shared_ptr<nexusfs::app::NexusFsService>
create_service(
    const std::filesystem::path& storage_root
)
{
    return std::make_shared<
        nexusfs::app::NexusFsService
    >(
        storage_root,
        1024
    );
}

nexusfs::http::HttpServer
create_server(
    const std::filesystem::path& storage_root
)
{
    const auto service =
        create_service(
            storage_root
        );

    const nexusfs::http::HttpRouter router{
        service
    };

    return nexusfs::http::HttpServer{
        "127.0.0.1",
        0,
        router
    };
}

class RunningServer final
{
public:
    explicit RunningServer(
        nexusfs::http::HttpServer& server
    )
        : server_{server}
    {
        thread_ =
            std::thread{
                [this]()
                {
                    try
                    {
                        server_.run();
                    }
                    catch (...)
                    {
                        const std::lock_guard lock{
                            exception_mutex_
                        };

                        exception_ =
                            std::current_exception();
                    }
                }
            };

        try
        {
            wait_until(
                [this]()
                {
                    return (
                        server_.is_running() ||
                        has_exception()
                    );
                },
                "HTTP server startup"
            );

            rethrow_if_failed();

            require_true(
                server_.is_running(),
                "HTTP server running-state test"
            );

            require_true(
                server_.port() != 0,
                "HTTP server ephemeral-port test"
            );
        }
        catch (...)
        {
            server_.stop();

            if (thread_.joinable())
            {
                thread_.join();
            }

            throw;
        }
    }

    RunningServer(
        const RunningServer&
    ) = delete;

    RunningServer& operator=(
        const RunningServer&
    ) = delete;

    ~RunningServer()
    {
        stop_and_join_noexcept();
    }

    void stop_and_join()
    {
        server_.stop();

        if (thread_.joinable())
        {
            thread_.join();
        }

        rethrow_if_failed();
    }

private:
    [[nodiscard]] bool
    has_exception() const
    {
        const std::lock_guard lock{
            exception_mutex_
        };

        return (
            exception_ != nullptr
        );
    }

    void rethrow_if_failed() const
    {
        std::exception_ptr exception;

        {
            const std::lock_guard lock{
                exception_mutex_
            };

            exception =
                exception_;
        }

        if (exception)
        {
            std::rethrow_exception(
                exception
            );
        }
    }

    void stop_and_join_noexcept() noexcept
    {
        server_.stop();

        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    nexusfs::http::HttpServer& server_;
    std::thread thread_;

    mutable std::mutex exception_mutex_;
    std::exception_ptr exception_;
};

nexusfs::http::HttpRouter::Response
send_health_request(
    std::uint16_t port
)
{
    asio::io_context io_context{
        1
    };

    Tcp::socket socket{
        io_context
    };

    const Tcp::endpoint endpoint{
        asio::ip::make_address(
            "127.0.0.1"
        ),
        port
    };

    socket.connect(
        endpoint
    );

    nexusfs::http::HttpRouter::Request request{
        beast_http::verb::get,
        "/api/v1/health",
        11
    };

    request.set(
        beast_http::field::host,
        "127.0.0.1"
    );

    request.set(
        beast_http::field::user_agent,
        "NexusFS lifecycle tests"
    );

    request.keep_alive(
        false
    );

    beast_http::write(
        socket,
        request
    );

    boost::beast::flat_buffer buffer;

    nexusfs::http::HttpRouter::Response response;

    beast_http::read(
        socket,
        buffer,
        response
    );

    boost::system::error_code ignored_error;

    socket.shutdown(
        Tcp::socket::shutdown_both,
        ignored_error
    );

    socket.close(
        ignored_error
    );

    return response;
}

Tcp::socket open_idle_connection(
    asio::io_context& io_context,
    std::uint16_t port
)
{
    Tcp::socket socket{
        io_context
    };

    const Tcp::endpoint endpoint{
        asio::ip::make_address(
            "127.0.0.1"
        ),
        port
    };

    socket.connect(
        endpoint
    );

    return socket;
}

void test_constructor_metadata()
{
    TemporaryDirectory directory;

    auto server =
        create_server(
            directory.path()
            / "storage"
        );

    require_equal(
        server.address(),
        std::string{"127.0.0.1"},
        "HTTP server address metadata test"
    );

    require_equal(
        server.port(),
        static_cast<std::uint16_t>(0),
        "HTTP server requested-port metadata test"
    );

    require_true(
        !server.is_running(),
        "HTTP server initial running-state test"
    );

    require_true(
        !server.stop_requested(),
        "HTTP server initial stop-state test"
    );

    require_equal(
        server.active_session_count(),
        static_cast<std::size_t>(0),
        "HTTP server initial session-count test"
    );
}

void test_real_health_request_and_shutdown()
{
    TemporaryDirectory directory;

    auto server =
        create_server(
            directory.path()
            / "storage"
        );

    RunningServer running_server{
        server
    };

    const auto response =
        send_health_request(
            server.port()
        );

    require_equal(
        response.result(),
        beast_http::status::ok,
        "HTTP server health-response status test"
    );

    require_equal(
        response.body(),
        std::string{
            "{\"status\":\"healthy\","
            "\"service\":\"nexusfs\","
            "\"api_version\":\"v1\"}"
        },
        "HTTP server health-response body test"
    );

    wait_until(
        [&server]()
        {
            return (
                server.active_session_count()
                == 0
            );
        },
        "HTTP completed-session cleanup"
    );

    running_server.stop_and_join();

    require_true(
        server.stop_requested(),
        "HTTP server requested-stop test"
    );

    require_true(
        !server.is_running(),
        "HTTP server stopped running-state test"
    );

    require_equal(
        server.active_session_count(),
        static_cast<std::size_t>(0),
        "HTTP server stopped session-count test"
    );
}

void test_active_session_interruption()
{
    TemporaryDirectory directory;

    auto server =
        create_server(
            directory.path()
            / "storage"
        );

    RunningServer running_server{
        server
    };

    asio::io_context client_context{
        1
    };

    Tcp::socket idle_socket =
        open_idle_connection(
            client_context,
            server.port()
        );

    wait_until(
        [&server]()
        {
            return (
                server.active_session_count()
                == 1
            );
        },
        "HTTP active-session registration"
    );

    server.stop();

    /*
     * stop() is deliberately idempotent.
     */
    server.stop();
    server.stop();

    running_server.stop_and_join();

    require_true(
        server.stop_requested(),
        "HTTP interrupted-session stop-state test"
    );

    require_true(
        !server.is_running(),
        "HTTP interrupted-session running-state test"
    );

    require_equal(
        server.active_session_count(),
        static_cast<std::size_t>(0),
        "HTTP interrupted-session cleanup test"
    );

    boost::system::error_code read_error;

    std::array<char, 1> buffer{};

    idle_socket.read_some(
        asio::buffer(buffer),
        read_error
    );

    require_true(
        static_cast<bool>(read_error),
        "HTTP interrupted client-connection test"
    );

    boost::system::error_code ignored_error;

    idle_socket.close(
        ignored_error
    );
}

void test_stop_before_run()
{
    TemporaryDirectory directory;

    auto server =
        create_server(
            directory.path()
            / "storage"
        );

    server.stop();
    server.stop();

    require_true(
        server.stop_requested(),
        "HTTP pre-run stop-state test"
    );

    server.run();

    require_true(
        !server.is_running(),
        "HTTP pre-stopped run-state test"
    );

    require_equal(
        server.port(),
        static_cast<std::uint16_t>(0),
        "HTTP pre-stopped port test"
    );
}

void test_run_is_one_shot()
{
    TemporaryDirectory directory;

    auto server =
        create_server(
            directory.path()
            / "storage"
        );

    {
        RunningServer running_server{
            server
        };

        running_server.stop_and_join();
    }

    bool logic_error_thrown = false;

    try
    {
        server.run();
    }
    catch (const std::logic_error&)
    {
        logic_error_thrown = true;
    }

    require_true(
        logic_error_thrown,
        "HTTP server one-shot run test"
    );
}

}

int main()
{
    try
    {
        test_constructor_metadata();

        std::cout
            << "[PASS] HTTP server constructor metadata\n";

        test_real_health_request_and_shutdown();

        std::cout
            << "[PASS] HTTP server real health request\n";

        test_active_session_interruption();

        std::cout
            << "[PASS] HTTP server active-session shutdown\n";

        test_stop_before_run();

        std::cout
            << "[PASS] HTTP server pre-run shutdown\n";

        test_run_is_one_shot();

        std::cout
            << "[PASS] HTTP server one-shot lifecycle\n";

        std::cout
            << "All NexusFS HTTP server lifecycle tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "[FAIL] "
            << error.what()
            << '\n';

        return 1;
    }
}