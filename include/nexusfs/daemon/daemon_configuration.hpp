#ifndef NEXUSFS_DAEMON_DAEMON_CONFIGURATION_HPP
#define NEXUSFS_DAEMON_DAEMON_CONFIGURATION_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace nexusfs::daemon
{

struct DaemonConfiguration
{
    std::string address{
        "127.0.0.1"
    };

    std::uint16_t port{
        8080
    };

    std::filesystem::path storage_root{
        "nexusfs_data"
    };

    std::size_t chunk_size{
        1024
    };
};

struct DaemonCommandLineResult
{
    DaemonConfiguration configuration;
    bool show_help{false};
};

[[nodiscard]] DaemonCommandLineResult
parse_daemon_command_line(
    int argc,
    const char* const* argv
);

[[nodiscard]] std::string
daemon_usage();

}

#endif