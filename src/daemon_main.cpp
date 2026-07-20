#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/daemon/daemon_configuration.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/system/error_code.hpp>

#include <csignal>
#include <cstddef>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

void register_shutdown_signals(
    boost::asio::signal_set& signals
)
{
    signals.add(
        SIGINT
    );

    signals.add(
        SIGTERM
    );

#ifdef SIGBREAK
    /*
     * Windows generates SIGBREAK for Ctrl + Break.
     */
    signals.add(
        SIGBREAK
    );
#endif
}

void stop_signal_context(
    boost::asio::io_context& signal_context,
    std::thread& signal_thread
) noexcept
{
    signal_context.stop();

    if (signal_thread.joinable())
    {
        signal_thread.join();
    }
}

}

int main(
    int argc,
    char* argv[]
)
{
    try
    {
        std::vector<const char*> arguments;

        arguments.reserve(
            static_cast<std::size_t>(
                argc
            )
        );

        for (
            int index = 0;
            index < argc;
            ++index
        )
        {
            arguments.push_back(
                argv[index]
            );
        }

        const nexusfs::daemon::DaemonCommandLineResult
            command_line =
                nexusfs::daemon::
                    parse_daemon_command_line(
                        argc,
                        arguments.data()
                    );

        if (command_line.show_help)
        {
            std::cout
                << nexusfs::daemon::
                    daemon_usage();

            return 0;
        }

        const nexusfs::daemon::DaemonConfiguration&
            configuration =
                command_line.configuration;

        std::cout
            << "NexusFS daemon configuration:\n"
            << "  Address: "
            << configuration.address
            << '\n'
            << "  Port: "
            << configuration.port
            << '\n'
            << "  Storage root: "
            << std::quoted(
                configuration.storage_root.string()
            )
            << '\n'
            << "  Chunk size: "
            << configuration.chunk_size
            << " bytes\n";

        const auto service =
            std::make_shared<
                nexusfs::app::NexusFsService
            >(
                configuration.storage_root,
                configuration.chunk_size
            );

        const nexusfs::http::HttpRouter router{
            service
        };

        nexusfs::http::HttpServer server{
            configuration.address,
            configuration.port,
            router
        };

        boost::asio::io_context signal_context{
            1
        };

        boost::asio::signal_set signals{
            signal_context
        };

        register_shutdown_signals(
            signals
        );

        signals.async_wait(
            [
                &server
            ](
                const boost::system::error_code& error,
                int signal_number
            )
            {
                if (error)
                {
                    return;
                }

                std::cout
                    << "\nNexusFS daemon received "
                    << "shutdown signal "
                    << signal_number
                    << ".\n";

                server.stop();
            }
        );

        std::thread signal_thread{
            [
                &signal_context
            ]()
            {
                signal_context.run();
            }
        };

        try
        {
            server.run();
        }
        catch (...)
        {
            server.stop();

            stop_signal_context(
                signal_context,
                signal_thread
            );

            throw;
        }

        stop_signal_context(
            signal_context,
            signal_thread
        );

        std::cout
            << "NexusFS daemon stopped cleanly.\n";

        return 0;
    }
    catch (const std::invalid_argument& error)
    {
        std::cerr
            << "NexusFS daemon configuration error: "
            << error.what()
            << "\n\n"
            << nexusfs::daemon::
                daemon_usage();

        return 1;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "NexusFS daemon error: "
            << error.what()
            << '\n';

        return 1;
    }
}