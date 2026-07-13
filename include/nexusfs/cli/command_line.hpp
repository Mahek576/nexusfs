#ifndef NEXUSFS_CLI_COMMAND_LINE_HPP
#define NEXUSFS_CLI_COMMAND_LINE_HPP

#include <filesystem>
#include <string>
#include <variant>

namespace nexusfs::cli
{

struct StoreCommand
{
    std::filesystem::path source_path;
};

struct RestoreCommand
{
    std::string manifest_id;
    std::filesystem::path output_path;
};

using Command = std::variant<
    StoreCommand,
    RestoreCommand
>;

class CommandLineParser
{
public:
    [[nodiscard]] static Command parse(
        int argument_count,
        char* argument_values[]
    );

    [[nodiscard]] static std::string usage();
};

}

#endif