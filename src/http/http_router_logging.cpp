#include "nexusfs/http/http_router.hpp"

#include <stdexcept>
#include <utility>

namespace nexusfs::http
{

HttpRouter::HttpRouter(
    std::shared_ptr<
        const app::NexusFsService
    > service,
    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry,
    std::shared_ptr<
        observability::JsonLogger
    > logger
)
    : HttpRouter{
          std::move(service),
          std::move(metrics_registry)
      }
{
    if (!logger)
    {
        throw std::invalid_argument(
            "HTTP router logger cannot be null."
        );
    }

    logger_ =
        std::move(logger);
}

observability::JsonLogger&
HttpRouter::logger() const noexcept
{
    return *logger_;
}

}