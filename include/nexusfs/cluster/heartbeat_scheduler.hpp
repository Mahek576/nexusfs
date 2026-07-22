#ifndef NEXUSFS_CLUSTER_HEARTBEAT_SCHEDULER_HPP
#define NEXUSFS_CLUSTER_HEARTBEAT_SCHEDULER_HPP

#include "nexusfs/cluster/cluster_node_foundation.hpp"

#include <chrono>
#include <memory>

namespace nexusfs::observability
{

class JsonLogger;
class MetricsRegistry;

}

namespace nexusfs::cluster
{

/*
 * Runs immediate and periodic outbound peer-heartbeat sweeps.
 *
 * stop() interrupts the interval wait and joins the worker thread.
 * A heartbeat currently inside network I/O is bounded by the
 * configured transport timeout.
 */
class HeartbeatScheduler final
{
public:
    HeartbeatScheduler(
        std::shared_ptr<
            ClusterNodeFoundation
        > cluster_node,
        std::shared_ptr<
            observability::MetricsRegistry
        > metrics_registry,
        std::shared_ptr<
            observability::JsonLogger
        > logger,
        std::chrono::milliseconds interval =
            std::chrono::milliseconds{
                0
            },
        std::chrono::milliseconds transport_timeout =
            std::chrono::milliseconds{
                3000
            }
    );

    ~HeartbeatScheduler();

    HeartbeatScheduler(
        const HeartbeatScheduler&
    ) = delete;

    HeartbeatScheduler& operator=(
        const HeartbeatScheduler&
    ) = delete;

    void start();

    void stop() noexcept;

    [[nodiscard]] bool
    is_running() const noexcept;

private:
    struct State;

    std::unique_ptr<State> state_;

    void run();
};

}

#endif
