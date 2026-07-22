#include "nexusfs/cluster/heartbeat_scheduler.hpp"

#include "nexusfs/cluster/peer_transport.hpp"
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

struct HeartbeatScheduler::State
{
    std::shared_ptr<
        ClusterNodeFoundation
    > cluster_node;

    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry;

    std::shared_ptr<
        observability::JsonLogger
    > logger;

    std::chrono::milliseconds interval;
    std::chrono::milliseconds transport_timeout;

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

HeartbeatScheduler::HeartbeatScheduler(
    std::shared_ptr<
        ClusterNodeFoundation
    > cluster_node,
    std::shared_ptr<
        observability::MetricsRegistry
    > metrics_registry,
    std::shared_ptr<
        observability::JsonLogger
    > logger,
    std::chrono::milliseconds interval,
    std::chrono::milliseconds transport_timeout
)
    : state_{
          std::make_unique<State>()
      }
{
    if (!cluster_node)
    {
        throw std::invalid_argument(
            "Heartbeat scheduler cluster node "
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
        interval ==
        std::chrono::milliseconds::zero()
    )
    {
        interval =
            std::chrono::milliseconds{
                cluster_node
                    ->configuration()
                    .heartbeat_interval_ms
            };
    }

    if (
        interval <=
        std::chrono::milliseconds::zero()
    )
    {
        throw std::invalid_argument(
            "Heartbeat scheduler interval "
            "must be positive."
        );
    }

    if (
        transport_timeout <=
        std::chrono::milliseconds::zero()
    )
    {
        throw std::invalid_argument(
            "Heartbeat transport timeout "
            "must be positive."
        );
    }

    state_->cluster_node =
        std::move(cluster_node);

    state_->metrics_registry =
        std::move(metrics_registry);

    state_->logger =
        std::move(logger);

    state_->interval =
        interval;

    state_->transport_timeout =
        transport_timeout;
}

HeartbeatScheduler::~HeartbeatScheduler()
{
    stop();
}

void HeartbeatScheduler::start()
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

void HeartbeatScheduler::stop() noexcept
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

bool HeartbeatScheduler::is_running() const noexcept
{
    return state_->running.load(
        std::memory_order_acquire
    );
}

void HeartbeatScheduler::run()
{
    state_->running.store(
        true,
        std::memory_order_release
    );

    state_->logger->log(
        observability::LogLevel::info,
        "heartbeat_scheduler_started",
        "NexusFS heartbeat scheduler started.",
        {
            observability::LogField{
                "interval_ms",
                static_cast<std::uint64_t>(
                    state_->interval.count()
                )
            },
            observability::LogField{
                "transport_timeout_ms",
                static_cast<std::uint64_t>(
                    state_->
                        transport_timeout.count()
                )
            },
            observability::LogField{
                "configured_peers",
                static_cast<std::uint64_t>(
                    state_->cluster_node
                        ->configuration()
                        .peers.size()
                )
            }
        }
    );

    PeerTransport transport{
        state_->cluster_node,
        state_->transport_timeout
    };

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

        for (
            const PeerDefinition& peer :
            state_->cluster_node
                ->configuration()
                .peers
        )
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
                transport.send_heartbeat(
                    peer
                );

                state_->metrics_registry->
                    record_heartbeat_attempt(
                        true
                    );

                state_->logger->log(
                    observability::LogLevel::debug,
                    "peer_heartbeat_succeeded",
                    "Outbound peer heartbeat succeeded.",
                    {
                        observability::LogField{
                            "peer_node_id",
                            peer.node_id
                        },
                        observability::LogField{
                            "peer_address",
                            peer.address
                        },
                        observability::LogField{
                            "peer_port",
                            static_cast<std::uint64_t>(
                                peer.port
                            )
                        }
                    }
                );
            }
            catch (const std::exception& error)
            {
                state_->metrics_registry->
                    record_heartbeat_attempt(
                        false
                    );

                state_->logger->log(
                    observability::LogLevel::warning,
                    "peer_heartbeat_failed",
                    "Outbound peer heartbeat failed.",
                    {
                        observability::LogField{
                            "peer_node_id",
                            peer.node_id
                        },
                        observability::LogField{
                            "peer_address",
                            peer.address
                        },
                        observability::LogField{
                            "peer_port",
                            static_cast<std::uint64_t>(
                                peer.port
                            )
                        },
                        observability::LogField{
                            "error",
                            error.what()
                        }
                    }
                );
            }
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

    state_->logger->log(
        observability::LogLevel::info,
        "heartbeat_scheduler_stopped",
        "NexusFS heartbeat scheduler stopped."
    );
}

}
