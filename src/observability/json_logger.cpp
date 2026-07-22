#include "nexusfs/observability/json_logger.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace nexusfs::observability
{

namespace
{

std::string_view level_name(
    LogLevel level
) noexcept
{
    switch (level)
    {
        case LogLevel::debug:
            return "debug";

        case LogLevel::info:
            return "info";

        case LogLevel::warning:
            return "warning";

        case LogLevel::error:
            return "error";
    }

    return "unknown";
}

std::string utc_timestamp()
{
    using Clock =
        std::chrono::system_clock;

    const Clock::time_point now =
        Clock::now();

    const std::time_t current_time =
        Clock::to_time_t(
            now
        );

    std::tm utc_time{};

#ifdef _WIN32
    if (
        gmtime_s(
            &utc_time,
            &current_time
        ) != 0
    )
    {
        throw std::runtime_error(
            "Failed to create UTC log timestamp."
        );
    }
#else
    if (
        gmtime_r(
            &current_time,
            &utc_time
        ) == nullptr
    )
    {
        throw std::runtime_error(
            "Failed to create UTC log timestamp."
        );
    }
#endif

    const auto milliseconds =
        std::chrono::duration_cast<
            std::chrono::milliseconds
        >(
            now.time_since_epoch()
        )
        % std::chrono::seconds{
            1
        };

    std::ostringstream timestamp;

    timestamp
        << std::put_time(
               &utc_time,
               "%Y-%m-%dT%H:%M:%S"
           )
        << '.'
        << std::setw(3)
        << std::setfill('0')
        << milliseconds.count()
        << 'Z';

    return timestamp.str();
}

std::uint64_t current_thread_token() noexcept
{
    return static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(
            std::this_thread::get_id()
        )
    );
}

void assign_log_value(
    nlohmann::ordered_json& fields,
    const LogField& field
)
{
    std::visit(
        [
            &fields,
            &field
        ](
            const auto& value
        )
        {
            fields[field.name] =
                value;
        },
        field.value
    );
}

}

struct JsonLogger::State
{
    explicit State(
        std::ostream* output_stream
    )
        : output{
              output_stream
          }
    {
    }

    std::ostream* output;
    std::mutex output_mutex;
};

JsonLogger::JsonLogger(
    std::ostream* output
)
    : state_{
          std::make_unique<State>(
              output
          )
      }
{
}

JsonLogger::~JsonLogger() = default;

void JsonLogger::log(
    LogLevel level,
    std::string_view event,
    std::string_view message,
    std::initializer_list<LogField> fields
) noexcept
{
    if (!state_->output)
    {
        return;
    }

    try
    {
        nlohmann::ordered_json
            encoded_fields =
                nlohmann::ordered_json::object();

        for (
            const LogField& field :
            fields
        )
        {
            if (!field.name.empty())
            {
                assign_log_value(
                    encoded_fields,
                    field
                );
            }
        }

        const nlohmann::ordered_json payload = {
            {
                "timestamp",
                utc_timestamp()
            },
            {
                "level",
                level_name(
                    level
                )
            },
            {
                "event",
                event
            },
            {
                "message",
                message
            },
            {
                "thread",
                current_thread_token()
            },
            {
                "fields",
                std::move(
                    encoded_fields
                )
            }
        };

        const std::lock_guard lock{
            state_->output_mutex
        };

        *state_->output
            << payload.dump()
            << '\n';

        state_->output->flush();
    }
    catch (...)
    {
        /*
         * Operational logging must never break the operation
         * being observed.
         */
    }
}

bool JsonLogger::enabled() const noexcept
{
    return state_->output != nullptr;
}

}