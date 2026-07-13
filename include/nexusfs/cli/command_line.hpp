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

struct InspectCommand
{
    std::string manifest_id;
};

struct VerifyCommand
{
    std::string manifest_id;
};

using Command = std::variant<
    StoreCommand,
    RestoreCommand,
    InspectCommand,
    VerifyCommand
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