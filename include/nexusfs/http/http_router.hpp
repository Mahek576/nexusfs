#ifndef NEXUSFS_HTTP_HTTP_ROUTER_HPP
#define NEXUSFS_HTTP_HTTP_ROUTER_HPP

#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cluster/cluster_node_foundation.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"

#include <boost/beast/http.hpp>

#include <memory>
#include <string_view>

namespace nexusfs::http
{

class HttpRouter
{
public:
    using Request =
        boost::beast::http::request<
            boost::beast::http::string_body
        >;

    using Response =
        boost::beast::http::response<
            boost::beast::http::string_body
        >;

    /*
     * Constructs a router with private, process-local
     * observability objects and no cluster services.
     */
    explicit HttpRouter(
        std::shared_ptr<
            const app::NexusFsService
        > service
    );

    /*
     * Constructs a router with an explicitly shared metrics
     * registry and no cluster services.
     */
    HttpRouter(
        std::shared_ptr<
            const app::NexusFsService
        > service,
        std::shared_ptr<
            observability::MetricsRegistry
        > metrics_registry
    );

    /*
     * Constructs a router with daemon-wide observability and
     * no cluster services.
     */
    HttpRouter(
        std::shared_ptr<
            const app::NexusFsService
        > service,
        std::shared_ptr<
            observability::MetricsRegistry
        > metrics_registry,
        std::shared_ptr<
            observability::JsonLogger
        > logger
    );

    /*
     * Constructs a router with daemon-wide observability and
     * cluster services.
     */
    HttpRouter(
        std::shared_ptr<
            const app::NexusFsService
        > service,
        std::shared_ptr<
            observability::MetricsRegistry
        > metrics_registry,
        std::shared_ptr<
            observability::JsonLogger
        > logger,
        std::shared_ptr<
            cluster::ClusterNodeFoundation
        > cluster_node
    );

    [[nodiscard]] Response route(
        const Request& request
    ) const;

    [[nodiscard]] std::string_view
    normalized_route(
        const Request& request
    ) const noexcept;

    [[nodiscard]] observability::MetricsRegistry&
    metrics_registry() const noexcept;

    [[nodiscard]] observability::JsonLogger&
    logger() const noexcept;

private:
    [[nodiscard]] Response route_application(
        const Request& request
    ) const;

    [[nodiscard]] Response
    route_cluster_request(
        const Request& request
    ) const;

    [[nodiscard]] Response
    route_cluster_manifest_request(
        const Request& request
    ) const;

    void record_operation_metrics(
        const Request& request,
        const Response& response
    ) const noexcept;

    void refresh_storage_catalog_metrics()
        const noexcept;

    std::shared_ptr<
        const app::NexusFsService
    > service_;

    /*
     * Null for library routers that do not enable cluster APIs.
     */
    std::shared_ptr<
        cluster::ClusterNodeFoundation
    > cluster_node_;

    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry_{
        std::make_shared<
            observability::MetricsRegistry
        >()
    };

    /*
     * Library callers remain silent by default. The daemon injects
     * a logger connected to stdout.
     */
    std::shared_ptr<
        observability::JsonLogger
    > logger_{
        std::make_shared<
            observability::JsonLogger
        >()
    };
};

}

#endif