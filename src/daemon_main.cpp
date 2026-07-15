#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{

const std::filesystem::path storage_root{
    "nexusfs_data"
};

constexpr std::size_t default_chunk_size =
    1024;

std::uint16_t parse_port(
    std::string_view port_text
)
{
    if (port_text.empty())
    {
        throw std::invalid_argument(
            "HTTP server port cannot be empty."
        );
    }

    unsigned int parsed_port = 0;

    const char* const begin =
        port_text.data();

    const char* const end =
        begin + port_text.size();

    const auto [position, error] =
        std::from_chars(
            begin,
            end,
            parsed_port
        );

    if (
        error != std::errc{} ||
        position != end ||
        parsed_port == 0 ||
        parsed_port >
            std::numeric_limits<
                std::uint16_t
            >::max()
    )
    {
        throw std::invalid_argument(
            "HTTP server port must be an integer "
            "between 1 and 65535."
        );
    }

    return static_cast<std::uint16_t>(
        parsed_port
    );
}

void print_usage()
{
    std::cerr
        << "Usage:\n"
        << "  nexusfsd <address> <port>\n\n"
        << "Example:\n"
        << "  nexusfsd 127.0.0.1 8080\n";
}

}

int main(int argc, char* argv[])
{
    try
    {
        if (argc != 3)
        {
            print_usage();
            return 1;
        }

        std::string address{
            argv[1]
        };

        const std::uint16_t port =
            parse_port(
                argv[2]
            );

        const auto service =
            std::make_shared<
                nexusfs::app::NexusFsService
            >(
                storage_root,
                default_chunk_size
            );

        const nexusfs::http::HttpRouter router{
            service
        };

        const nexusfs::http::HttpServer server{
            std::move(address),
            port,
            router
        };

        server.run();

        return 0;
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