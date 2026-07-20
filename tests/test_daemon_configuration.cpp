#include "nexusfs/daemon/daemon_configuration.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

void require_true(
    bool condition,
    const std::string& test_name
)
{
    if (!condition)
    {
        throw std::runtime_error(
            test_name + " failed."
        );
    }
}

template <typename Actual, typename Expected>
void require_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& test_name
)
{
    if (actual != expected)
    {
        throw std::runtime_error(
            test_name + " failed."
        );
    }
}

nexusfs::daemon::DaemonCommandLineResult
parse_arguments(
    const std::vector<std::string>& arguments
)
{
    std::vector<const char*> raw_arguments;

    raw_arguments.reserve(
        arguments.size()
    );

    for (
        const std::string& argument :
        arguments
    )
    {
        raw_arguments.push_back(
            argument.c_str()
        );
    }

    return nexusfs::daemon::
        parse_daemon_command_line(
            static_cast<int>(
                raw_arguments.size()
            ),
            raw_arguments.data()
        );
}

template <typename Function>
void require_invalid_argument(
    Function&& function,
    const std::string& test_name
)
{
    bool exception_thrown = false;

    try
    {
        std::forward<Function>(
            function
        )();
    }
    catch (const std::invalid_argument&)
    {
        exception_thrown = true;
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(
            test_name
            + " failed with an unexpected exception: "
            + error.what()
        );
    }

    if (!exception_thrown)
    {
        throw std::runtime_error(
            test_name
            + " failed because no "
            "std::invalid_argument was thrown."
        );
    }
}

void test_default_configuration()
{
    const auto result =
        parse_arguments(
            {
                "nexusfsd"
            }
        );

    require_true(
        !result.show_help,
        "Default help-state test"
    );

    require_equal(
        result.configuration.address,
        std::string{"127.0.0.1"},
        "Default address test"
    );

    require_equal(
        result.configuration.port,
        static_cast<std::uint16_t>(8080),
        "Default port test"
    );

    require_equal(
        result.configuration.storage_root,
        std::filesystem::path{
            "nexusfs_data"
        },
        "Default storage-root test"
    );

    require_equal(
        result.configuration.chunk_size,
        static_cast<std::size_t>(1024),
        "Default chunk-size test"
    );
}

void test_custom_configuration()
{
    const auto result =
        parse_arguments(
            {
                "nexusfsd",
                "--address",
                "0.0.0.0",
                "--port",
                "9090",
                "--storage-root",
                "custom data",
                "--chunk-size",
                "1048576"
            }
        );

    require_true(
        !result.show_help,
        "Custom help-state test"
    );

    require_equal(
        result.configuration.address,
        std::string{"0.0.0.0"},
        "Custom address test"
    );

    require_equal(
        result.configuration.port,
        static_cast<std::uint16_t>(9090),
        "Custom port test"
    );

    require_equal(
        result.configuration.storage_root,
        std::filesystem::path{
            "custom data"
        },
        "Custom storage-root test"
    );

    require_equal(
        result.configuration.chunk_size,
        static_cast<std::size_t>(1048576),
        "Custom chunk-size test"
    );
}

void test_partial_configuration()
{
    const auto result =
        parse_arguments(
            {
                "nexusfsd",
                "--port",
                "8181",
                "--chunk-size",
                "4096"
            }
        );

    require_equal(
        result.configuration.address,
        std::string{"127.0.0.1"},
        "Partial configuration default-address test"
    );

    require_equal(
        result.configuration.port,
        static_cast<std::uint16_t>(8181),
        "Partial configuration port test"
    );

    require_equal(
        result.configuration.storage_root,
        std::filesystem::path{
            "nexusfs_data"
        },
        "Partial configuration storage-root test"
    );

    require_equal(
        result.configuration.chunk_size,
        static_cast<std::size_t>(4096),
        "Partial configuration chunk-size test"
    );
}

void test_option_order_independence()
{
    const auto result =
        parse_arguments(
            {
                "nexusfsd",
                "--chunk-size",
                "4096",
                "--storage-root",
                "ordered-data",
                "--port",
                "8181",
                "--address",
                "127.0.0.2"
            }
        );

    require_equal(
        result.configuration.address,
        std::string{"127.0.0.2"},
        "Option-order address test"
    );

    require_equal(
        result.configuration.port,
        static_cast<std::uint16_t>(8181),
        "Option-order port test"
    );

    require_equal(
        result.configuration.storage_root,
        std::filesystem::path{
            "ordered-data"
        },
        "Option-order storage-root test"
    );

    require_equal(
        result.configuration.chunk_size,
        static_cast<std::size_t>(4096),
        "Option-order chunk-size test"
    );
}

void test_legacy_configuration()
{
    const auto result =
        parse_arguments(
            {
                "nexusfsd",
                "127.0.0.1",
                "7070"
            }
        );

    require_equal(
        result.configuration.address,
        std::string{"127.0.0.1"},
        "Legacy address test"
    );

    require_equal(
        result.configuration.port,
        static_cast<std::uint16_t>(7070),
        "Legacy port test"
    );

    require_equal(
        result.configuration.storage_root,
        std::filesystem::path{
            "nexusfs_data"
        },
        "Legacy storage-root default test"
    );

    require_equal(
        result.configuration.chunk_size,
        static_cast<std::size_t>(1024),
        "Legacy chunk-size default test"
    );
}

void test_ipv6_configuration()
{
    const auto named_result =
        parse_arguments(
            {
                "nexusfsd",
                "--address",
                "::1"
            }
        );

    require_equal(
        named_result.configuration.address,
        std::string{"::1"},
        "Named IPv6 address test"
    );

    const auto legacy_result =
        parse_arguments(
            {
                "nexusfsd",
                "::1",
                "8080"
            }
        );

    require_equal(
        legacy_result.configuration.address,
        std::string{"::1"},
        "Legacy IPv6 address test"
    );
}

void test_help_options()
{
    const auto long_help =
        parse_arguments(
            {
                "nexusfsd",
                "--help"
            }
        );

    require_true(
        long_help.show_help,
        "Long help-option test"
    );

    const auto short_help =
        parse_arguments(
            {
                "nexusfsd",
                "-h"
            }
        );

    require_true(
        short_help.show_help,
        "Short help-option test"
    );
}

void test_invalid_ports()
{
    const std::vector<std::string> invalid_ports{
        "",
        "0",
        "65536",
        "999999",
        "abc",
        "8080x",
        "-1",
        "+8080",
        "80.80"
    };

    for (
        const std::string& port :
        invalid_ports
    )
    {
        require_invalid_argument(
            [&port]()
            {
                static_cast<void>(
                    parse_arguments(
                        {
                            "nexusfsd",
                            "--port",
                            port
                        }
                    )
                );
            },
            "Invalid port test: "
                + port
        );
    }
}

void test_invalid_chunk_sizes()
{
    const std::vector<std::string>
        invalid_chunk_sizes{
            "",
            "0",
            "abc",
            "1024x",
            "-1",
            "+1024",
            "10.5",
            "18446744073709551616"
        };

    for (
        const std::string& chunk_size :
        invalid_chunk_sizes
    )
    {
        require_invalid_argument(
            [&chunk_size]()
            {
                static_cast<void>(
                    parse_arguments(
                        {
                            "nexusfsd",
                            "--chunk-size",
                            chunk_size
                        }
                    )
                );
            },
            "Invalid chunk-size test: "
                + chunk_size
        );
    }
}

void test_invalid_addresses()
{
    const std::vector<std::string> invalid_addresses{
        "",
        " ",
        "localhost",
        "999.999.999.999",
        "127.0.0",
        "not-an-address",
        "http://127.0.0.1"
    };

    for (
        const std::string& address :
        invalid_addresses
    )
    {
        require_invalid_argument(
            [&address]()
            {
                static_cast<void>(
                    parse_arguments(
                        {
                            "nexusfsd",
                            "--address",
                            address
                        }
                    )
                );
            },
            "Invalid address test: "
                + address
        );
    }
}

void test_invalid_storage_roots()
{
    const std::vector<std::string> invalid_roots{
        "",
        " ",
        "\t",
        "\r\n"
    };

    for (
        const std::string& storage_root :
        invalid_roots
    )
    {
        require_invalid_argument(
            [&storage_root]()
            {
                static_cast<void>(
                    parse_arguments(
                        {
                            "nexusfsd",
                            "--storage-root",
                            storage_root
                        }
                    )
                );
            },
            "Invalid storage-root test"
        );
    }
}

void test_missing_option_values()
{
    const std::vector<std::string> options{
        "--address",
        "--port",
        "--storage-root",
        "--chunk-size"
    };

    for (
        const std::string& option :
        options
    )
    {
        require_invalid_argument(
            [&option]()
            {
                static_cast<void>(
                    parse_arguments(
                        {
                            "nexusfsd",
                            option
                        }
                    )
                );
            },
            "Missing option-value test: "
                + option
        );
    }

    require_invalid_argument(
        []()
        {
            static_cast<void>(
                parse_arguments(
                    {
                        "nexusfsd",
                        "--port",
                        "--chunk-size",
                        "1024"
                    }
                )
            );
        },
        "Option-used-as-value test"
    );
}

void test_duplicate_options()
{
    const std::vector<
        std::vector<std::string>
    > duplicate_argument_sets{
        {
            "nexusfsd",
            "--address",
            "127.0.0.1",
            "--address",
            "0.0.0.0"
        },
        {
            "nexusfsd",
            "--port",
            "8080",
            "--port",
            "9090"
        },
        {
            "nexusfsd",
            "--storage-root",
            "data-one",
            "--storage-root",
            "data-two"
        },
        {
            "nexusfsd",
            "--chunk-size",
            "1024",
            "--chunk-size",
            "2048"
        }
    };

    for (
        const auto& arguments :
        duplicate_argument_sets
    )
    {
        require_invalid_argument(
            [&arguments]()
            {
                static_cast<void>(
                    parse_arguments(
                        arguments
                    )
                );
            },
            "Duplicate daemon-option test"
        );
    }
}

void test_unknown_and_positional_arguments()
{
    require_invalid_argument(
        []()
        {
            static_cast<void>(
                parse_arguments(
                    {
                        "nexusfsd",
                        "--unknown",
                        "value"
                    }
                )
            );
        },
        "Unknown option test"
    );

    require_invalid_argument(
        []()
        {
            static_cast<void>(
                parse_arguments(
                    {
                        "nexusfsd",
                        "unexpected"
                    }
                )
            );
        },
        "Unexpected positional argument test"
    );

    require_invalid_argument(
        []()
        {
            static_cast<void>(
                parse_arguments(
                    {
                        "nexusfsd",
                        "127.0.0.1",
                        "8080",
                        "extra"
                    }
                )
            );
        },
        "Excess legacy argument test"
    );

    require_invalid_argument(
        []()
        {
            static_cast<void>(
                parse_arguments(
                    {
                        "nexusfsd",
                        "127.0.0.1",
                        "--port"
                    }
                )
            );
        },
        "Mixed positional and named arguments test"
    );
}

void test_help_combination_rejection()
{
    require_invalid_argument(
        []()
        {
            static_cast<void>(
                parse_arguments(
                    {
                        "nexusfsd",
                        "--help",
                        "--port",
                        "8080"
                    }
                )
            );
        },
        "Long help combination test"
    );

    require_invalid_argument(
        []()
        {
            static_cast<void>(
                parse_arguments(
                    {
                        "nexusfsd",
                        "-h",
                        "--address",
                        "127.0.0.1"
                    }
                )
            );
        },
        "Short help combination test"
    );
}

void test_invalid_parser_inputs()
{
    require_invalid_argument(
        []()
        {
            static_cast<void>(
                nexusfs::daemon::
                    parse_daemon_command_line(
                        0,
                        nullptr
                    )
            );
        },
        "Unavailable command-line test"
    );

    const char* invalid_arguments[]{
        "nexusfsd",
        nullptr
    };

    require_invalid_argument(
        [&invalid_arguments]()
        {
            static_cast<void>(
                nexusfs::daemon::
                    parse_daemon_command_line(
                        2,
                        invalid_arguments
                    )
            );
        },
        "Null command-line argument test"
    );
}

void test_usage_text()
{
    const std::string usage =
        nexusfs::daemon::
            daemon_usage();

    const std::vector<std::string>
        required_fragments{
            "NexusFS daemon",
            "nexusfsd [options]",
            "nexusfsd <address> <port>",
            "--address",
            "--port",
            "--storage-root",
            "--chunk-size",
            "--help",
            "127.0.0.1",
            "8080",
            "nexusfs_data",
            "1024 bytes",
            "1048576"
        };

    for (
        const std::string& fragment :
        required_fragments
    )
    {
        require_true(
            usage.find(fragment) !=
                std::string::npos,
            "Usage fragment test: "
                + fragment
        );
    }
}

}

int main()
{
    try
    {
        test_default_configuration();

        std::cout
            << "[PASS] Daemon default configuration\n";

        test_custom_configuration();

        std::cout
            << "[PASS] Daemon custom configuration\n";

        test_partial_configuration();

        std::cout
            << "[PASS] Daemon partial configuration\n";

        test_option_order_independence();

        std::cout
            << "[PASS] Daemon option-order independence\n";

        test_legacy_configuration();

        std::cout
            << "[PASS] Daemon legacy configuration\n";

        test_ipv6_configuration();

        std::cout
            << "[PASS] Daemon IPv6 configuration\n";

        test_help_options();

        std::cout
            << "[PASS] Daemon help options\n";

        test_invalid_ports();

        std::cout
            << "[PASS] Daemon invalid port rejection\n";

        test_invalid_chunk_sizes();

        std::cout
            << "[PASS] Daemon invalid chunk-size rejection\n";

        test_invalid_addresses();

        std::cout
            << "[PASS] Daemon invalid address rejection\n";

        test_invalid_storage_roots();

        std::cout
            << "[PASS] Daemon invalid storage-root rejection\n";

        test_missing_option_values();

        std::cout
            << "[PASS] Daemon missing option-value rejection\n";

        test_duplicate_options();

        std::cout
            << "[PASS] Daemon duplicate-option rejection\n";

        test_unknown_and_positional_arguments();

        std::cout
            << "[PASS] Daemon unknown argument rejection\n";

        test_help_combination_rejection();

        std::cout
            << "[PASS] Daemon help combination rejection\n";

        test_invalid_parser_inputs();

        std::cout
            << "[PASS] Daemon invalid parser-input rejection\n";

        test_usage_text();

        std::cout
            << "[PASS] Daemon usage documentation\n";

        std::cout
            << "All NexusFS daemon configuration tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "[FAIL] "
            << error.what()
            << '\n';

        return 1;
    }
}