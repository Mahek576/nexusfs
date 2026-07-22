#ifndef NEXUSFS_OBSERVABILITY_JSON_LOGGER_HPP
#define NEXUSFS_OBSERVABILITY_JSON_LOGGER_HPP

#include <cstdint>
#include <initializer_list>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace nexusfs::observability
{

enum class LogLevel
{
    debug,
    info,
    warning,
    error
};

using LogValue =
    std::variant<
        std::string,
        std::uint64_t,
        std::int64_t,
        double,
        bool
    >;

struct LogField
{
    LogField(
        std::string field_name,
        std::string field_value
    )
        : name{
              std::move(field_name)
          },
          value{
              std::move(field_value)
          }
    {
    }

    LogField(
        std::string field_name,
        const char* field_value
    )
        : name{
              std::move(field_name)
          },
          value{
              std::string{
                  field_value == nullptr
                      ? ""
                      : field_value
              }
          }
    {
    }

    LogField(
        std::string field_name,
        std::uint64_t field_value
    )
        : name{
              std::move(field_name)
          },
          value{
              field_value
          }
    {
    }

    LogField(
        std::string field_name,
        std::int64_t field_value
    )
        : name{
              std::move(field_name)
          },
          value{
              field_value
          }
    {
    }

    LogField(
        std::string field_name,
        double field_value
    )
        : name{
              std::move(field_name)
          },
          value{
              field_value
          }
    {
    }

    LogField(
        std::string field_name,
        bool field_value
    )
        : name{
              std::move(field_name)
          },
          value{
              field_value
          }
    {
    }

    std::string name;
    LogValue value;
};

/*
 * Thread-safe newline-delimited JSON logger.
 *
 * A null output stream creates a disabled logger. This allows
 * library tests and embedded NexusFS users to avoid unsolicited
 * console output while the daemon can inject an enabled logger.
 *
 * Logging is best-effort and noexcept. Serialization or stream
 * failures never alter storage or HTTP behavior.
 */
class JsonLogger final
{
public:
    explicit JsonLogger(
        std::ostream* output = nullptr
    );

    ~JsonLogger();

    JsonLogger(
        const JsonLogger&
    ) = delete;

    JsonLogger& operator=(
        const JsonLogger&
    ) = delete;

    JsonLogger(
        JsonLogger&&
    ) = delete;

    JsonLogger& operator=(
        JsonLogger&&
    ) = delete;

    void log(
        LogLevel level,
        std::string_view event,
        std::string_view message,
        std::initializer_list<LogField> fields = {}
    ) noexcept;

    [[nodiscard]] bool
    enabled() const noexcept;

private:
    struct State;

    std::unique_ptr<State> state_;
};

}

#endif