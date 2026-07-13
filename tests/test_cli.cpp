#include "nexusfs/cli/command_line.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
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

template <typename Function>
void require_throws(
    Function&& function,
    const std::string& test_name
)
{
    bool exception_thrown = false;

    try
    {
        std::forward<Function>(function)();
    }
    catch (const std::exception&)
    {
        exception_thrown = true;
    }

    if (!exception_thrown)
    {
        throw std::runtime_error(
            test_name
            + " failed because no exception was thrown."
        );
    }
}

nexusfs::cli::Command parse_arguments(
    std::vector<std::string> arguments
)
{
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(
        arguments.size()
    );

    for (std::string& argument : arguments)
    {
        argument_pointers.push_back(
            argument.data()
        );
    }

    return nexusfs::cli::CommandLineParser::parse(
        static_cast<int>(
            argument_pointers.size()
        ),
        argument_pointers.data()
    );
}

std::string valid_manifest_id(
    char hexadecimal_character = 'a'
)
{
    return std::string(
        64,
        hexadecimal_character
    );
}

void test_store_command()
{
    const auto command =
        parse_arguments(
            {
                "nexusfs",
                "store",
                "example.bin"
            }
        );

    const auto* store_command =
        std::get_if<nexusfs::cli::StoreCommand>(
            &command
        );

    require_true(
        store_command != nullptr,
        "Store command type test"
    );

    require_equal(
        store_command->source_path,
        std::filesystem::path{"example.bin"},
        "Store source-path test"
    );
}

void test_restore_command()
{
    const std::string manifest_id =
        valid_manifest_id('a');

    const auto command =
        parse_arguments(
            {
                "nexusfs",
                "restore",
                manifest_id,
                "restored.bin"
            }
        );

    const auto* restore_command =
        std::get_if<nexusfs::cli::RestoreCommand>(
            &command
        );

    require_true(
        restore_command != nullptr,
        "Restore command type test"
    );

    require_equal(
        restore_command->manifest_id,
        manifest_id,
        "Restore manifest-ID test"
    );

    require_equal(
        restore_command->output_path,
        std::filesystem::path{"restored.bin"},
        "Restore output-path test"
    );
}

void test_inspect_command()
{
    const std::string manifest_id =
        valid_manifest_id('b');

    const auto command =
        parse_arguments(
            {
                "nexusfs",
                "inspect",
                manifest_id
            }
        );

    const auto* inspect_command =
        std::get_if<nexusfs::cli::InspectCommand>(
            &command
        );

    require_true(
        inspect_command != nullptr,
        "Inspect command type test"
    );

    require_equal(
        inspect_command->manifest_id,
        manifest_id,
        "Inspect manifest-ID test"
    );
}

void test_verify_command()
{
    const std::string manifest_id =
        valid_manifest_id('c');

    const auto command =
        parse_arguments(
            {
                "nexusfs",
                "verify",
                manifest_id
            }
        );

    const auto* verify_command =
        std::get_if<nexusfs::cli::VerifyCommand>(
            &command
        );

    require_true(
        verify_command != nullptr,
        "Verify command type test"
    );

    require_equal(
        verify_command->manifest_id,
        manifest_id,
        "Verify manifest-ID test"
    );
}

void test_list_command()
{
    const auto command =
        parse_arguments(
            {
                "nexusfs",
                "list"
            }
        );

    require_true(
        std::holds_alternative<
            nexusfs::cli::ListCommand
        >(command),
        "List command type test"
    );
}

void test_missing_command_rejected()
{
    require_throws(
        []()
        {
            (void)parse_arguments(
                {
                    "nexusfs"
                }
            );
        },
        "Missing command rejection test"
    );
}

void test_unknown_command_rejected()
{
    require_throws(
        []()
        {
            (void)parse_arguments(
                {
                    "nexusfs",
                    "unknown"
                }
            );
        },
        "Unknown command rejection test"
    );
}

void test_invalid_manifest_id_rejected()
{
    require_throws(
        []()
        {
            (void)parse_arguments(
                {
                    "nexusfs",
                    "verify",
                    "invalid-id"
                }
            );
        },
        "Invalid manifest-ID rejection test"
    );
}

void test_uppercase_manifest_id_rejected()
{
    require_throws(
        []()
        {
            (void)parse_arguments(
                {
                    "nexusfs",
                    "inspect",
                    valid_manifest_id('A')
                }
            );
        },
        "Uppercase manifest-ID rejection test"
    );
}

void test_store_argument_count_rejected()
{
    require_throws(
        []()
        {
            (void)parse_arguments(
                {
                    "nexusfs",
                    "store"
                }
            );
        },
        "Store argument-count rejection test"
    );
}

void test_restore_argument_count_rejected()
{
    require_throws(
        []()
        {
            (void)parse_arguments(
                {
                    "nexusfs",
                    "restore",
                    valid_manifest_id('a')
                }
            );
        },
        "Restore argument-count rejection test"
    );
}

void test_list_extra_argument_rejected()
{
    require_throws(
        []()
        {
            (void)parse_arguments(
                {
                    "nexusfs",
                    "list",
                    "unexpected"
                }
            );
        },
        "List extra-argument rejection test"
    );
}

}

int main()
{
    try
    {
        test_store_command();

        std::cout
            << "[PASS] Store command parsing\n";

        test_restore_command();

        std::cout
            << "[PASS] Restore command parsing\n";

        test_inspect_command();

        std::cout
            << "[PASS] Inspect command parsing\n";

        test_verify_command();

        std::cout
            << "[PASS] Verify command parsing\n";

        test_list_command();

        std::cout
            << "[PASS] List command parsing\n";

        test_missing_command_rejected();

        std::cout
            << "[PASS] Missing command rejection\n";

        test_unknown_command_rejected();

        std::cout
            << "[PASS] Unknown command rejection\n";

        test_invalid_manifest_id_rejected();

        std::cout
            << "[PASS] Invalid manifest ID rejection\n";

        test_uppercase_manifest_id_rejected();

        std::cout
            << "[PASS] Uppercase manifest ID rejection\n";

        test_store_argument_count_rejected();

        std::cout
            << "[PASS] Store argument-count rejection\n";

        test_restore_argument_count_rejected();

        std::cout
            << "[PASS] Restore argument-count rejection\n";

        test_list_extra_argument_rejected();

        std::cout
            << "[PASS] List extra-argument rejection\n";

        std::cout
            << "All NexusFS CLI tests passed.\n";

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