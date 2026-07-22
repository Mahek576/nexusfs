#include "nexusfs/cluster/replica_maintenance_scheduler.hpp"

#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace nexusfs::cluster
{

struct ReplicaMaintenanceScheduler::State
{
    std::shared_ptr<
        const app::NexusFsService
    > service;

    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry;

    std::shared_ptr<
        observability::JsonLogger
    > logger;

    std::chrono::milliseconds interval;

    mutable std::mutex mutex;
    std::condition_variable condition;

    bool stop_requested{
        false
    };

    std::atomic<bool> running{
        false
    };

    std::thread worker;
};

ReplicaMaintenanceScheduler::
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
    std::chrono::milliseconds interval
)
    : state_{
          std::make_unique<State>()
      }
{
    if (!service)
    {
        throw std::invalid_argument(
            "Replica-maintenance scheduler service "
            "cannot be null."
        );
    }

    if (!metrics_registry)
    {
        metrics_registry =
            std::make_shared<
                observability::MetricsRegistry
            >();
    }

    if (!logger)
    {
        logger =
            std::make_shared<
                observability::JsonLogger
            >();
    }

    if (
        interval <
        std::chrono::milliseconds{
            100
        }
    )
    {
        throw std::invalid_argument(
            "Replica-maintenance scheduler interval "
            "must be at least 100 milliseconds."
        );
    }

    state_->service =
        std::move(service);

    state_->metrics_registry =
        std::move(metrics_registry);

    state_->logger =
        std::move(logger);

    state_->interval =
        interval;
}

ReplicaMaintenanceScheduler::
~ReplicaMaintenanceScheduler()
{
    stop();
}

void ReplicaMaintenanceScheduler::start()
{
    const std::lock_guard lock{
        state_->mutex
    };

    if (
        state_->worker.joinable()
        || state_->running.load(
            std::memory_order_acquire
        )
    )
    {
        return;
    }

    state_->stop_requested =
        false;

    state_->worker =
        std::thread{
            [
                this
            ]()
            {
                run();
            }
        };
}

void ReplicaMaintenanceScheduler::stop()
    noexcept
{
    try
    {
        {
            const std::lock_guard lock{
                state_->mutex
            };

            state_->stop_requested =
                true;
        }

        state_->condition.notify_all();

        if (state_->worker.joinable())
        {
            state_->worker.join();
        }
    }
    catch (...)
    {
    }

    state_->running.store(
        false,
        std::memory_order_release
    );
}

bool ReplicaMaintenanceScheduler::is_running()
    const noexcept
{
    return state_->running.load(
        std::memory_order_acquire
    );
}

void ReplicaMaintenanceScheduler::run()
{
    state_->running.store(
        true,
        std::memory_order_release
    );

    state_->metrics_registry->
        record_replica_maintenance_scheduler_started();

    state_->logger->log(
        observability::LogLevel::info,
        "replica_maintenance_scheduler_started",
        "NexusFS replica-maintenance scheduler started.",
        {
            observability::LogField{
                "interval_ms",
                static_cast<std::uint64_t>(
                    state_->interval.count()
                )
            }
        }
    );

    for (;;)
    {
        {
            const std::lock_guard lock{
                state_->mutex
            };

            if (state_->stop_requested)
            {
                break;
            }
        }

        try
        {
            const app::RepairReplicasResult result =
                state_->service->
                    repair_replicas();

            state_->logger->log(
                result.fully_repaired
                    ? observability::LogLevel::info
                    : observability::LogLevel::warning,
                "replica_maintenance_scheduler_sweep_completed",
                result.fully_repaired
                    ? "Scheduled replica-maintenance sweep "
                      "completed successfully."
                    : "Scheduled replica-maintenance sweep "
                      "completed with under-replicated chunks.",
                {
                    observability::LogField{
                        "manifests_scanned",
                        static_cast<std::uint64_t>(
                            result.manifests_scanned
                        )
                    },
                    observability::LogField{
                        "unique_chunks_scanned",
                        static_cast<std::uint64_t>(
                            result.unique_chunks_scanned
                        )
                    },
                    observability::LogField{
                        "local_chunks_recovered",
                        static_cast<std::uint64_t>(
                            result.local_chunks_recovered
                        )
                    },
                    observability::LogField{
                        "remote_replicas_observed",
                        static_cast<std::uint64_t>(
                            result.remote_replicas_observed
                        )
                    },
                    observability::LogField{
                        "remote_replicas_created",
                        static_cast<std::uint64_t>(
                            result.remote_replicas_created
                        )
                    },
                    observability::LogField{
                        "peer_failures",
                        static_cast<std::uint64_t>(
                            result.peer_failures
                        )
                    },
                    observability::LogField{
                        "under_replicated_chunks",
                        static_cast<std::uint64_t>(
                            result.under_replicated_chunks
                        )
                    },
                    observability::LogField{
                        "fully_repaired",
                        result.fully_repaired
                    }
                }
            );
        }
        catch (const std::exception& error)
        {
            state_->metrics_registry->
                record_replica_maintenance_scheduler_failure();

            state_->logger->log(
                observability::LogLevel::error,
                "replica_maintenance_scheduler_sweep_failed",
                "Scheduled replica-maintenance sweep failed.",
                {
                    observability::LogField{
                        "error",
                        error.what()
                    }
                }
            );
        }

        std::unique_lock lock{
            state_->mutex
        };

        if (
            state_->condition.wait_for(
                lock,
                state_->interval,
                [
                    this
                ]()
                {
                    return state_->
                        stop_requested;
                }
            )
        )
        {
            break;
        }
    }

    state_->running.store(
        false,
        std::memory_order_release
    );

    state_->metrics_registry->
        record_replica_maintenance_scheduler_stopped();

    state_->logger->log(
        observability::LogLevel::info,
        "replica_maintenance_scheduler_stopped",
        "NexusFS replica-maintenance scheduler stopped."
    );
}

}
