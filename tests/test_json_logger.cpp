#include "nexusfs/observability/json_logger.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

using nexusfs::observability::
    JsonLogger;

using nexusfs::observability::
    LogField;

using nexusfs::observability::
    LogLevel;

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

template <
    typename Actual,
    typename Expected
>
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

std::vector<nlohmann::json>
parse_log_lines(
    const std::string& encoded_logs
)
{
    std::istringstream input{
        encoded_logs
    };

    std::vector<nlohmann::json> logs;
    std::string line;

    while (
        std::getline(
            input,
            line
        )
    )
    {
        if (line.empty())
        {
            continue;
        }

        const nlohmann::json payload =
            nlohmann::json::parse(
                line,
                nullptr,
                false
            );

        require_true(
            !payload.is_discarded(),
            "JSON log parsing test"
        );

        require_true(
            payload.is_object(),
            "JSON log object test"
        );

        logs.push_back(
            payload
        );
    }

    return logs;
}

void test_disabled_logger()
{
    JsonLogger logger;

    require_true(
        !logger.enabled(),
        "Disabled logger state test"
    );

    logger.log(
        LogLevel::info,
        "disabled_event",
        "This must not throw."
    );
}

void test_typed_json_log()
{
    std::ostringstream output;

    JsonLogger logger{
        &output
    };

    require_true(
        logger.enabled(),
        "Enabled logger state test"
    );

    logger.log(
        LogLevel::warning,
        "request_completed",
        "HTTP request completed.",
        {
            LogField{
                "method",
                "GET"
            },
            LogField{
                "status_code",
                static_cast<
                    std::uint64_t
                >(
                    404
                )
            },
            LogField{
                "latency_microseconds",
                static_cast<
                    std::uint64_t
                >(
                    125
                )
            },
            LogField{
                "successful",
                false
            },
            LogField{
                "ratio",
                1.5
            }
        }
    );

    const auto logs =
        parse_log_lines(
            output.str()
        );

    require_equal(
        logs.size(),
        static_cast<std::size_t>(
            1
        ),
        "Single JSON log test"
    );

    const nlohmann::json& payload =
        logs.front();

    require_equal(
        payload.at("level")
            .get<std::string>(),
        std::string{
            "warning"
        },
        "JSON log level test"
    );

    require_equal(
        payload.at("event")
            .get<std::string>(),
        std::string{
            "request_completed"
        },
        "JSON log event test"
    );

    require_true(
        payload.at("timestamp")
            .is_string(),
        "JSON log timestamp test"
    );

    require_true(
        payload.at("thread")
            .is_number_unsigned(),
        "JSON log thread test"
    );

    const nlohmann::json& fields =
        payload.at("fields");

    require_equal(
        fields.at("method")
            .get<std::string>(),
        std::string{
            "GET"
        },
        "JSON string-field test"
    );

    require_equal(
        fields.at("status_code")
            .get<std::uint64_t>(),
        static_cast<std::uint64_t>(
            404
        ),
        "JSON integer-field test"
    );

    require_equal(
        fields.at("successful")
            .get<bool>(),
        false,
        "JSON Boolean-field test"
    );

    require_equal(
        fields.at("ratio")
            .get<double>(),
        1.5,
        "JSON floating-field test"
    );
}

void test_concurrent_logging()
{
    constexpr std::size_t thread_count =
        8;

    constexpr std::size_t logs_per_thread =
        200;

    std::ostringstream output;

    JsonLogger logger{
        &output
    };

    std::vector<std::thread> workers;

    workers.reserve(
        thread_count
    );

    for (
        std::size_t worker_index = 0;
        worker_index < thread_count;
        ++worker_index
    )
    {
        workers.emplace_back(
            [
                &logger,
                worker_index
            ]()
            {
                for (
                    std::size_t log_index = 0;
                    log_index < logs_per_thread;
                    ++log_index
                )
                {
                    logger.log(
                        LogLevel::info,
                        "concurrent_event",
                        "Concurrent structured log.",
                        {
                            LogField{
                                "worker",
                                static_cast<
                                    std::uint64_t
                                >(
                                    worker_index
                                )
                            },
                            LogField{
                                "sequence",
                                static_cast<
                                    std::uint64_t
                                >(
                                    log_index
                                )
                            }
                        }
                    );
                }
            }
        );
    }

    for (
        std::thread& worker :
        workers
    )
    {
        worker.join();
    }

    const auto logs =
        parse_log_lines(
            output.str()
        );

    require_equal(
        logs.size(),
        thread_count
            * logs_per_thread,
        "Concurrent JSON log-count test"
    );

    std::set<std::string>
        observed_records;

    for (
        const nlohmann::json& payload :
        logs
    )
    {
        require_equal(
            payload.at("event")
                .get<std::string>(),
            std::string{
                "concurrent_event"
            },
            "Concurrent event-name test"
        );

        const nlohmann::json& fields =
            payload.at("fields");

        const std::string record =
            std::to_string(
                fields.at("worker")
                    .get<std::uint64_t>()
            )
            + ":"
            + std::to_string(
                fields.at("sequence")
                    .get<std::uint64_t>()
            );

        observed_records.insert(
            record
        );
    }

    require_equal(
        observed_records.size(),
        thread_count
            * logs_per_thread,
        "Concurrent unique-log test"
    );
}

}

int main()
{
    try
    {
        test_disabled_logger();

        std::cout
            << "[PASS] JSON logger disabled mode\n";

        test_typed_json_log();

        std::cout
            << "[PASS] JSON logger typed fields\n";

        test_concurrent_logging();

        std::cout
            << "[PASS] JSON logger concurrent output\n";

        std::cout
            << "All NexusFS JSON logger tests passed.\n";

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