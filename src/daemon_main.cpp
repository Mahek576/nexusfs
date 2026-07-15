#include "nexusfs/http/http_server.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{

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

        const nexusfs::http::HttpServer server{
            std::move(address),
            port
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