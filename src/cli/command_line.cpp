#include "nexusfs/cli/command_line.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace nexusfs::cli
{

namespace
{

bool is_valid_manifest_id(
    const std::string& manifest_id
)
{
    constexpr std::size_t sha256_hex_length = 64;

    if (manifest_id.size() != sha256_hex_length)
    {
        return false;
    }

    for (const char character : manifest_id)
    {
        const bool is_decimal_digit =
            character >= '0' &&
            character <= '9';

        const bool is_lowercase_hexadecimal =
            character >= 'a' &&
            character <= 'f';

        if (
            !is_decimal_digit &&
            !is_lowercase_hexadecimal
        )
        {
            return false;
        }
    }

    return true;
}

}

Command CommandLineParser::parse(
    int argument_count,
    char* argument_values[]
)
{
    if (
        argument_count < 2 ||
        argument_values == nullptr
    )
    {
        throw std::invalid_argument(
            "No NexusFS command was provided.\n\n"
            + usage()
        );
    }

    const std::string command_name{
        argument_values[1]
    };

    if (command_name == "store")
    {
        if (argument_count != 3)
        {
            throw std::invalid_argument(
                "The store command requires exactly one file path.\n\n"
                + usage()
            );
        }

        std::filesystem::path source_path{
            argument_values[2]
        };

        if (source_path.empty())
        {
            throw std::invalid_argument(
                "The store source path cannot be empty."
            );
        }

        return StoreCommand{
            std::move(source_path)
        };
    }

    if (command_name == "restore")
    {
        if (argument_count != 4)
        {
            throw std::invalid_argument(
                "The restore command requires a manifest ID "
                "and an output path.\n\n"
                + usage()
            );
        }

        std::string manifest_id{
            argument_values[2]
        };

        if (!is_valid_manifest_id(manifest_id))
        {
            throw std::invalid_argument(
                "The restore manifest ID must contain exactly "
                "64 lowercase hexadecimal characters."
            );
        }

        std::filesystem::path output_path{
            argument_values[3]
        };

        if (output_path.empty())
        {
            throw std::invalid_argument(
                "The restore output path cannot be empty."
            );
        }

        return RestoreCommand{
            std::move(manifest_id),
            std::move(output_path)
        };
    }

    throw std::invalid_argument(
        "Unknown NexusFS command: "
        + command_name
        + "\n\n"
        + usage()
    );
}

std::string CommandLineParser::usage()
{
    return
        "Usage:\n"
        "  nexusfs store <file-path>\n"
        "  nexusfs restore <manifest-id> <output-path>";
}

}