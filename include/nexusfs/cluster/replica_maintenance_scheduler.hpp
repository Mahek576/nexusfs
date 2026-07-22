#ifndef NEXUSFS_CLUSTER_REPLICA_MAINTENANCE_SCHEDULER_HPP
#define NEXUSFS_CLUSTER_REPLICA_MAINTENANCE_SCHEDULER_HPP

#include <chrono>
#include <memory>

namespace nexusfs::app
{

class NexusFsService;

}

namespace nexusfs::observability
{

class JsonLogger;
class MetricsRegistry;

}

namespace nexusfs::cluster
{

/*
 * Runs an immediate replica-maintenance sweep and then repeats at
 * the configured interval.
 *
 * stop() interrupts the interval wait and joins the worker thread.
 * A sweep already executing is allowed to finish so that storage
 * locks and durable publication operations are not abandoned.
 */
class ReplicaMaintenanceScheduler final
{
public:
    ReplicaMaintenanceScheduler(
        std::shared_ptr<
            const app::NexusFsService
        > service,
        std::shared_ptr<
            observability::MetricsRegistry
        > metrics_registry,
        std::shared_ptr<
            observability::JsonLogger
        > logger,
        std::chrono::milliseconds interval =
            std::chrono::milliseconds{
                30000
            }
    );

    ~ReplicaMaintenanceScheduler();

    ReplicaMaintenanceScheduler(
        const ReplicaMaintenanceScheduler&
    ) = delete;

    ReplicaMaintenanceScheduler& operator=(
        const ReplicaMaintenanceScheduler&
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
