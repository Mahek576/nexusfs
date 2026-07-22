#include "nexusfs/admin/admin_client.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

struct Options
{
    std::string command;
    std::string address{
        "127.0.0.1"
    };

    std::uint16_t port{
        8080
    };

    std::string token;

    std::string operation_id;
    std::uint64_t membership_epoch{0};
};

std::string usage()
{
    return
        "NexusFS administrator CLI\n"
        "\n"
        "Usage:\n"
        "  nexusfsctl status [options]\n"
        "  nexusfsctl files [options]\n"
        "  nexusfsctl sync [options]\n"
        "  nexusfsctl repair [options]\n"
        "  nexusfsctl maintenance [options]\n"
        "  nexusfsctl rebalance <operation-id> <membership-epoch> [options]\n"
        "\n"
        "Options:\n"
        "  --address <ip>   Admin API address. Default: 127.0.0.1\n"
        "  --port <port>    Admin API port. Default: 8080\n"
        "  --token <token>  Admin bearer token. Prefer NEXUSFS_ADMIN_TOKEN.\n"
        "  -h, --help       Display this message.\n";
}

std::uint64_t parse_unsigned(
    std::string_view value,
    std::string_view field
)
{
    std::uint64_t result =
        0;

    const char* const begin =
        value.data();

    const char* const end =
        begin + value.size();

    const auto [position, error] =
        std::from_chars(
            begin,
            end,
            result
        );

    if (
        error != std::errc{}
        || position != end
    )
    {
        throw std::invalid_argument(
            std::string{
                field
            }
            + " must be an unsigned decimal integer."
        );
    }

    return result;
}

Options parse_options(
    int argc,
    char* argv[]
)
{
    if (
        argc < 2
        || argv == nullptr
    )
    {
        throw std::invalid_argument(
            "No administrator command was supplied.\n\n"
            + usage()
        );
    }

    const std::string first{
        argv[1]
    };

    if (
        first == "-h"
        || first == "--help"
    )
    {
        Options options;
        options.command = "help";

        return options;
    }

    Options options;
    options.command = first;

    int index =
        2;

    if (options.command == "rebalance")
    {
        if (argc < 4)
        {
            throw std::invalid_argument(
                "The rebalance command requires an "
                "operation ID and membership epoch.\n\n"
                + usage()
            );
        }

        options.operation_id =
            argv[index++];

        options.membership_epoch =
            parse_unsigned(
                argv[index++],
                "Membership epoch"
            );

        if (
            options.operation_id.empty()
            || options.membership_epoch == 0
        )
        {
            throw std::invalid_argument(
                "Rebalance operation ID and epoch "
                "must be non-empty and positive."
            );
        }
    }
    else if (
        options.command != "status"
        && options.command != "files"
        && options.command != "sync"
        && options.command != "repair"
        && options.command != "maintenance"
    )
    {
        throw std::invalid_argument(
            "Unknown administrator command: "
            + options.command
            + "\n\n"
            + usage()
        );
    }

    bool address_seen =
        false;

    bool port_seen =
        false;

    bool token_seen =
        false;

    while (index < argc)
    {
        const std::string argument{
            argv[index++]
        };

        if (
            argument == "-h"
            || argument == "--help"
        )
        {
            throw std::invalid_argument(
                "Help cannot be combined with "
                "an administrator command."
            );
        }

        if (index >= argc)
        {
            throw std::invalid_argument(
                "Missing value for administrator option "
                + argument
                + "."
            );
        }

        const std::string value{
            argv[index++]
        };

        if (argument == "--address")
        {
            if (address_seen)
            {
                throw std::invalid_argument(
                    "--address was supplied more than once."
                );
            }

            address_seen =
                true;

            options.address =
                value;
        }
        else if (argument == "--port")
        {
            if (port_seen)
            {
                throw std::invalid_argument(
                    "--port was supplied more than once."
                );
            }

            port_seen =
                true;

            const std::uint64_t parsed =
                parse_unsigned(
                    value,
                    "Port"
                );

            if (
                parsed == 0
                || parsed >
                    std::numeric_limits<
                        std::uint16_t
                    >::max()
            )
            {
                throw std::invalid_argument(
                    "Port must be between 1 and 65535."
                );
            }

            options.port =
                static_cast<std::uint16_t>(
                    parsed
                );
        }
        else if (argument == "--token")
        {
            if (token_seen)
            {
                throw std::invalid_argument(
                    "--token was supplied more than once."
                );
            }

            token_seen =
                true;

            options.token =
                value;
        }
        else
        {
            throw std::invalid_argument(
                "Unknown administrator option: "
                + argument
                + "."
            );
        }
    }

    if (options.token.empty())
    {
        const char* const environment_token =
            std::getenv(
                "NEXUSFS_ADMIN_TOKEN"
            );

        if (environment_token != nullptr)
        {
            options.token =
                environment_token;
        }
    }

    if (options.token.empty())
    {
        throw std::invalid_argument(
            "No admin token was provided. Set "
            "NEXUSFS_ADMIN_TOKEN or use --token."
        );
    }

    return options;
}

void print_response(
    const nexusfs::admin::AdminResponse& response
)
{
    const nlohmann::json payload =
        nlohmann::json::parse(
            response.body,
            nullptr,
            false
        );

    if (payload.is_discarded())
    {
        std::cout
            << response.body
            << '\n';

        return;
    }

    std::cout
        << payload.dump(2)
        << '\n';
}

}

int main(
    int argc,
    char* argv[]
)
{
    try
    {
        const Options options =
            parse_options(
                argc,
                argv
            );

        if (options.command == "help")
        {
            std::cout
                << usage();

            return 0;
        }

        const nexusfs::admin::AdminClient client{
            options.address,
            options.port,
            options.token
        };

        nexusfs::admin::AdminResponse response;

        if (options.command == "status")
        {
            response =
                client.get(
                    "/api/v1/admin/overview"
                );
        }
        else if (options.command == "files")
        {
            response =
                client.get(
                    "/api/v1/admin/files"
                );
        }
        else if (options.command == "sync")
        {
            response =
                client.post(
                    "/api/v1/admin/operations/catalog-sync"
                );
        }
        else if (options.command == "repair")
        {
            response =
                client.post(
                    "/api/v1/admin/operations/repair"
                );
        }
        else if (options.command == "maintenance")
        {
            response =
                client.post(
                    "/api/v1/admin/operations/maintenance"
                );
        }
        else
        {
            response =
                client.post(
                    "/api/v1/admin/operations/rebalance",
                    nlohmann::ordered_json{
                        {
                            "operation_id",
                            options.operation_id
                        },
                        {
                            "expected_membership_epoch",
                            options.membership_epoch
                        }
                    }.dump()
                );
        }

        print_response(
            response
        );

        return response.successful()
            ? 0
            : 1;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "nexusfsctl error: "
            << error.what()
            << '\n';

        return 1;
    }
}
