#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/daemon/daemon_configuration.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"

#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

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

        const nexusfs::http::HttpServer server{
            configuration.address,
            configuration.port,
            router
        };

        server.run();

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