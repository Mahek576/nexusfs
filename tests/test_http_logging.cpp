#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{

namespace asio =
    boost::asio;

namespace http =
    boost::beast::http;

using Tcp =
    asio::ip::tcp;

constexpr std::chrono::seconds timeout{
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

        path_ =
            std::filesystem::temp_directory_path()
            / (
                "nexusfs-http-logging-"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count()
                )
                + "-"
                + std::to_string(
                    sequence.fetch_add(
                        1,
                        std::memory_order_relaxed
                    )
                )
            );

        std::filesystem::create_directories(
            path_
        );
    }

    ~TemporaryDirectory()
    {
        std::error_code error;

        std::filesystem::remove_all(
            path_,
            error
        );
    }

    [[nodiscard]]
    const std::filesystem::path&
    path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void require_true(
    bool condition,
    const std::string& name
)
{
    if (!condition)
    {
        throw std::runtime_error(
            name + " failed."
        );
    }
}

template <typename Predicate>
void wait_until(
    Predicate&& predicate,
    const std::string& name
)
{
    const auto deadline =
        std::chrono::steady_clock::now()
        + timeout;

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
                name + " timed out."
            );
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds{
                5
            }
        );
    }
}

void send_request(
    std::uint16_t port,
    http::verb method,
    const std::string& target
)
{
    asio::io_context context{
        1
    };

    Tcp::socket socket{
        context
    };

    socket.connect(
        Tcp::endpoint{
            asio::ip::make_address(
                "127.0.0.1"
            ),
            port
        }
    );

    nexusfs::http::HttpRouter::Request request{
        method,
        target,
        11
    };

    request.set(
        http::field::host,
        "127.0.0.1"
    );

    request.keep_alive(
        false
    );

    request.prepare_payload();

    http::write(
        socket,
        request
    );

    boost::beast::flat_buffer buffer;

    nexusfs::http::HttpRouter::Response response;

    http::read(
        socket,
        buffer,
        response
    );
}

void test_http_structured_logging()
{
    TemporaryDirectory directory;

    std::ostringstream output;

    const auto logger =
        std::make_shared<
            nexusfs::observability::JsonLogger
        >(
            &output
        );

    const auto metrics =
        std::make_shared<
            nexusfs::observability::
                MetricsRegistry
        >();

    const auto service =
        std::make_shared<
            nexusfs::app::NexusFsService
        >(
            directory.path()
                / "storage",
            1024
        );

    const nexusfs::http::HttpRouter router{
        service,
        metrics,
        logger
    };

    nexusfs::http::HttpServer server{
        "127.0.0.1",
        0,
        router
    };

    std::exception_ptr server_exception;

    std::thread server_thread{
        [
            &server,
            &server_exception
        ]()
        {
            try
            {
                server.run();
            }
            catch (...)
            {
                server_exception =
                    std::current_exception();
            }
        }
    };

    wait_until(
        [&server]()
        {
            return server.is_running();
        },
        "HTTP logging server startup"
    );

    send_request(
        server.port(),
        http::verb::get,
        "/api/v1/health"
    );

    send_request(
        server.port(),
        http::verb::get,
        "/unknown"
    );

    wait_until(
        [&metrics]()
        {
            const auto snapshot =
                metrics->snapshot();

            return (
                snapshot.requests_total == 2
                && snapshot.requests_in_flight == 0
                && snapshot.connections_active == 0
            );
        },
        "HTTP logging request completion"
    );

    server.stop();
    server_thread.join();

    if (server_exception)
    {
        std::rethrow_exception(
            server_exception
        );
    }

    std::istringstream input{
        output.str()
    };

    std::string line;
    std::vector<nlohmann::json> logs;

    while (
        std::getline(
            input,
            line
        )
    )
    {
        if (!line.empty())
        {
            logs.push_back(
                nlohmann::json::parse(
                    line
                )
            );
        }
    }

    require_true(
        logs.size() == 2,
        "HTTP structured log-count test"
    );

    require_true(
        logs[0].at("event")
            == "http_request_completed",
        "HTTP success event test"
    );

    require_true(
        logs[0].at("fields").at("method")
            == "GET",
        "HTTP success method test"
    );

    require_true(
        logs[0].at("fields").at("route")
            == "/api/v1/health",
        "HTTP success route test"
    );

    require_true(
        logs[0].at("fields").at("status_code")
            == 200,
        "HTTP success status test"
    );

    require_true(
        logs[0].at("fields").at("successful")
            == true,
        "HTTP success outcome test"
    );

    require_true(
        logs[1].at("fields").at("route")
            == "/unmatched",
        "HTTP failure route test"
    );

    require_true(
        logs[1].at("fields").at("status_code")
            == 404,
        "HTTP failure status test"
    );

    require_true(
        logs[1].at("level")
            == "warning",
        "HTTP failure level test"
    );
}

}

int main()
{
    try
    {
        test_http_structured_logging();

        std::cout
            << "[PASS] HTTP structured request logging\n"
            << "All NexusFS HTTP logging tests passed.\n";

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