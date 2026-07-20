#include "nexusfs/daemon/daemon_configuration.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace nexusfs::daemon
{

namespace
{

constexpr std::string_view address_option{
    "--address"
};

constexpr std::string_view port_option{
    "--port"
};

constexpr std::string_view storage_root_option{
    "--storage-root"
};

constexpr std::string_view chunk_size_option{
    "--chunk-size"
};

constexpr std::string_view help_option{
    "--help"
};

constexpr std::string_view short_help_option{
    "-h"
};

std::string_view argument_at(
    int argc,
    const char* const* argv,
    int index
)
{
    if (
        index < 0 ||
        index >= argc ||
        argv[index] == nullptr
    )
    {
        throw std::invalid_argument(
            "The daemon command line contains "
            "an invalid argument."
        );
    }

    return std::string_view{
        argv[index]
    };
}

bool is_ascii_whitespace(
    char character
)
{
    return (
        character == ' ' ||
        character == '\t' ||
        character == '\r' ||
        character == '\n'
    );
}

bool contains_non_whitespace(
    std::string_view value
)
{
    for (char character : value)
    {
        if (!is_ascii_whitespace(character))
        {
            return true;
        }
    }

    return false;
}

bool contains_ascii_whitespace(
    std::string_view value
)
{
    for (char character : value)
    {
        if (is_ascii_whitespace(character))
        {
            return true;
        }
    }

    return false;
}

bool starts_with_option_prefix(
    std::string_view argument
)
{
    return (
        argument.starts_with("--") ||
        argument == short_help_option
    );
}

bool is_help_argument(
    std::string_view argument
)
{
    return (
        argument == help_option ||
        argument == short_help_option
    );
}

std::string_view read_option_value(
    int argc,
    const char* const* argv,
    int& index,
    std::string_view option
)
{
    const int value_index =
        index + 1;

    if (value_index >= argc)
    {
        throw std::invalid_argument(
            "Missing value for daemon option "
            + std::string{option}
            + "."
        );
    }

    const std::string_view value =
        argument_at(
            argc,
            argv,
            value_index
        );

    if (starts_with_option_prefix(value))
    {
        throw std::invalid_argument(
            "Missing value for daemon option "
            + std::string{option}
            + "."
        );
    }

    index = value_index;

    return value;
}

std::uint64_t parse_unsigned_integer(
    std::string_view value,
    std::string_view field_name
)
{
    if (value.empty())
    {
        throw std::invalid_argument(
            std::string{field_name}
            + " cannot be empty."
        );
    }

    std::uint64_t parsed_value = 0;

    const char* const begin =
        value.data();

    const char* const end =
        begin + value.size();

    const auto [position, error] =
        std::from_chars(
            begin,
            end,
            parsed_value
        );

    if (
        error != std::errc{} ||
        position != end
    )
    {
        throw std::invalid_argument(
            std::string{field_name}
            + " must be a decimal integer."
        );
    }

    return parsed_value;
}

std::uint16_t parse_port(
    std::string_view value
)
{
    const std::uint64_t parsed_port =
        parse_unsigned_integer(
            value,
            "HTTP server port"
        );

    if (
        parsed_port == 0 ||
        parsed_port >
            std::numeric_limits<
                std::uint16_t
            >::max()
    )
    {
        throw std::invalid_argument(
            "HTTP server port must be between "
            "1 and 65535."
        );
    }

    return static_cast<std::uint16_t>(
        parsed_port
    );
}

std::size_t parse_chunk_size(
    std::string_view value
)
{
    const std::uint64_t parsed_chunk_size =
        parse_unsigned_integer(
            value,
            "Chunk size"
        );

    if (parsed_chunk_size == 0)
    {
        throw std::invalid_argument(
            "Chunk size must be greater than zero."
        );
    }

    if (
        parsed_chunk_size >
        static_cast<std::uint64_t>(
            std::numeric_limits<
                std::size_t
            >::max()
        )
    )
    {
        throw std::invalid_argument(
            "Chunk size exceeds the supported "
            "platform limit."
        );
    }

    return static_cast<std::size_t>(
        parsed_chunk_size
    );
}

bool is_decimal_digit(
    char character
)
{
    return (
        character >= '0' &&
        character <= '9'
    );
}

bool is_valid_ipv4_octet(
    std::string_view octet
)
{
    if (
        octet.empty() ||
        octet.size() > 3
    )
    {
        return false;
    }

    /*
     * Reject ambiguous forms such as 001.002.003.004.
     * A multi-digit octet must not begin with zero.
     */
    if (
        octet.size() > 1 &&
        octet.front() == '0'
    )
    {
        return false;
    }

    for (char character : octet)
    {
        if (!is_decimal_digit(character))
        {
            return false;
        }
    }

    unsigned int parsed_octet = 0;

    const char* const begin =
        octet.data();

    const char* const end =
        begin + octet.size();

    const auto [position, error] =
        std::from_chars(
            begin,
            end,
            parsed_octet
        );

    return (
        error == std::errc{} &&
        position == end &&
        parsed_octet <= 255U
    );
}

bool is_strict_ipv4_address(
    std::string_view value
)
{
    std::size_t octet_begin = 0;
    std::size_t octet_count = 0;

    while (octet_begin <= value.size())
    {
        if (octet_count >= 4)
        {
            return false;
        }

        const std::size_t separator =
            value.find(
                '.',
                octet_begin
            );

        const std::size_t octet_end =
            separator == std::string_view::npos
                ? value.size()
                : separator;

        const std::string_view octet =
            value.substr(
                octet_begin,
                octet_end - octet_begin
            );

        if (!is_valid_ipv4_octet(octet))
        {
            return false;
        }

        ++octet_count;

        if (
            separator ==
            std::string_view::npos
        )
        {
            break;
        }

        octet_begin =
            separator + 1;
    }

    return octet_count == 4;
}

bool is_valid_ipv6_address(
    std::string_view value
)
{
    boost::system::error_code address_error;

    const boost::asio::ip::address parsed_address =
        boost::asio::ip::make_address(
            std::string{value},
            address_error
        );

    return (
        !address_error &&
        parsed_address.is_v6()
    );
}

std::string parse_address(
    std::string_view value
)
{
    if (
        value.empty() ||
        !contains_non_whitespace(value) ||
        contains_ascii_whitespace(value)
    )
    {
        throw std::invalid_argument(
            "HTTP server address cannot be empty "
            "or contain whitespace."
        );
    }

    bool valid_address = false;

    if (
        value.find(':') !=
        std::string_view::npos
    )
    {
        /*
         * IPv6 may use compressed notation such as ::1,
         * so Boost.Asio performs the IPv6 parsing.
         */
        valid_address =
            is_valid_ipv6_address(
                value
            );
    }
    else
    {
        /*
         * IPv4 must contain exactly four decimal octets.
         * This rejects abbreviated forms such as 127.0.0.
         */
        valid_address =
            is_strict_ipv4_address(
                value
            );
    }

    if (!valid_address)
    {
        throw std::invalid_argument(
            "HTTP server address must be a valid "
            "four-octet IPv4 address or numeric "
            "IPv6 address."
        );
    }

    return std::string{
        value
    };
}

std::filesystem::path parse_storage_root(
    std::string_view value
)
{
    if (
        value.empty() ||
        !contains_non_whitespace(value)
    )
    {
        throw std::invalid_argument(
            "Storage root cannot be empty."
        );
    }

    return std::filesystem::path{
        std::string{value}
    };
}

void reject_duplicate_option(
    bool& already_seen,
    std::string_view option
)
{
    if (already_seen)
    {
        throw std::invalid_argument(
            "Daemon option "
            + std::string{option}
            + " was provided more than once."
        );
    }

    already_seen = true;
}

bool is_legacy_invocation(
    int argc,
    const char* const* argv
)
{
    if (argc != 3)
    {
        return false;
    }

    const std::string_view first_argument =
        argument_at(
            argc,
            argv,
            1
        );

    const std::string_view second_argument =
        argument_at(
            argc,
            argv,
            2
        );

    return (
        !starts_with_option_prefix(
            first_argument
        ) &&
        !starts_with_option_prefix(
            second_argument
        )
    );
}

DaemonCommandLineResult
parse_legacy_command_line(
    int argc,
    const char* const* argv
)
{
    DaemonCommandLineResult result;

    result.configuration.address =
        parse_address(
            argument_at(
                argc,
                argv,
                1
            )
        );

    result.configuration.port =
        parse_port(
            argument_at(
                argc,
                argv,
                2
            )
        );

    return result;
}

}

DaemonCommandLineResult
parse_daemon_command_line(
    int argc,
    const char* const* argv
)
{
    if (
        argc < 1 ||
        argv == nullptr
    )
    {
        throw std::invalid_argument(
            "The daemon command line is unavailable."
        );
    }

    if (argc == 1)
    {
        return DaemonCommandLineResult{};
    }

    if (
        is_legacy_invocation(
            argc,
            argv
        )
    )
    {
        return parse_legacy_command_line(
            argc,
            argv
        );
    }

    DaemonCommandLineResult result;

    bool address_seen = false;
    bool port_seen = false;
    bool storage_root_seen = false;
    bool chunk_size_seen = false;

    for (
        int index = 1;
        index < argc;
        ++index
    )
    {
        const std::string_view argument =
            argument_at(
                argc,
                argv,
                index
            );

        if (is_help_argument(argument))
        {
            if (argc != 2)
            {
                throw std::invalid_argument(
                    "The help option cannot be combined "
                    "with other daemon options."
                );
            }

            result.show_help = true;

            return result;
        }

        if (argument == address_option)
        {
            reject_duplicate_option(
                address_seen,
                address_option
            );

            result.configuration.address =
                parse_address(
                    read_option_value(
                        argc,
                        argv,
                        index,
                        address_option
                    )
                );

            continue;
        }

        if (argument == port_option)
        {
            reject_duplicate_option(
                port_seen,
                port_option
            );

            result.configuration.port =
                parse_port(
                    read_option_value(
                        argc,
                        argv,
                        index,
                        port_option
                    )
                );

            continue;
        }

        if (argument == storage_root_option)
        {
            reject_duplicate_option(
                storage_root_seen,
                storage_root_option
            );

            result.configuration.storage_root =
                parse_storage_root(
                    read_option_value(
                        argc,
                        argv,
                        index,
                        storage_root_option
                    )
                );

            continue;
        }

        if (argument == chunk_size_option)
        {
            reject_duplicate_option(
                chunk_size_seen,
                chunk_size_option
            );

            result.configuration.chunk_size =
                parse_chunk_size(
                    read_option_value(
                        argc,
                        argv,
                        index,
                        chunk_size_option
                    )
                );

            continue;
        }

        if (starts_with_option_prefix(argument))
        {
            throw std::invalid_argument(
                "Unknown daemon option: "
                + std::string{argument}
                + "."
            );
        }

        throw std::invalid_argument(
            "Unexpected positional daemon argument: "
            + std::string{argument}
            + "."
        );
    }

    return result;
}

std::string daemon_usage()
{
    return
        "NexusFS daemon\n"
        "\n"
        "Usage:\n"
        "  nexusfsd [options]\n"
        "  nexusfsd <address> <port>\n"
        "\n"
        "Options:\n"
        "  --address <ip>\n"
        "      Numeric IPv4 or IPv6 address to bind.\n"
        "\n"
        "  --port <number>\n"
        "      TCP port between 1 and 65535.\n"
        "\n"
        "  --storage-root <path>\n"
        "      Directory used for chunks and manifests.\n"
        "\n"
        "  --chunk-size <bytes>\n"
        "      Fixed chunk size in bytes. Must be greater than zero.\n"
        "\n"
        "  -h, --help\n"
        "      Display this help message.\n"
        "\n"
        "Defaults:\n"
        "  Address:      127.0.0.1\n"
        "  Port:         8080\n"
        "  Storage root: nexusfs_data\n"
        "  Chunk size:   1024 bytes\n"
        "\n"
        "Examples:\n"
        "  nexusfsd\n"
        "  nexusfsd 127.0.0.1 8080\n"
        "  nexusfsd --address 0.0.0.0 --port 8080\n"
        "  nexusfsd --storage-root ./data --chunk-size 1048576\n";
}

}