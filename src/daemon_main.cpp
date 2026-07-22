#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/daemon/daemon_configuration.hpp"
#include "nexusfs/http/http_router.hpp"
#include "nexusfs/http/http_server.hpp"
#include "nexusfs/observability/json_logger.hpp"
#include "nexusfs/observability/metrics_registry.hpp"
#include "nexusfs/storage/storage_recovery.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/system/error_code.hpp>

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

void register_shutdown_signals(
    boost::asio::signal_set& signals
)
{
    signals.add(
        SIGINT
    );

    signals.add(
        SIGTERM
    );

#ifdef SIGBREAK
    signals.add(
        SIGBREAK
    );
#endif
}

void stop_signal_context(
    boost::asio::io_context& signal_context,
    std::thread& signal_thread
) noexcept
{
    signal_context.stop();

    if (signal_thread.joinable())
    {
        signal_thread.join();
    }
}

}

int main(
    int argc,
    char* argv[]
)
{
    try
    {
        std::vector<const char*> arguments;

        arguments.reserve(
            static_cast<std::size_t>(
                argc
            )
        );

        for (
            int index = 0;
            index < argc;
            ++index
        )
        {
            arguments.push_back(
                argv[index]
            );
        }

        const nexusfs::daemon::
            DaemonCommandLineResult command_line =
                nexusfs::daemon::
                    parse_daemon_command_line(
                        argc,
                        arguments.data()
                    );

        if (command_line.show_help)
        {
            std::cout
                << nexusfs::daemon::
                    daemon_usage();

            return 0;
        }

        const nexusfs::daemon::
            DaemonConfiguration& configuration =
                command_line.configuration;

        const auto logger =
            std::make_shared<
                nexusfs::observability::
                    JsonLogger
            >(
                &std::cout
            );

        const auto metrics_registry =
            std::make_shared<
                nexusfs::observability::
                    MetricsRegistry
            >();

        logger->log(
            nexusfs::observability::
                LogLevel::info,
            "daemon_configuration",
            "NexusFS daemon configuration loaded.",
            {
                nexusfs::observability::LogField{
                    "address",
                    configuration.address
                },
                nexusfs::observability::LogField{
                    "port",
                    static_cast<std::uint64_t>(
                        configuration.port
                    )
                },
                nexusfs::observability::LogField{
                    "storage_root",
                    configuration
                        .storage_root
                        .string()
                },
                nexusfs::observability::LogField{
                    "chunk_size",
                    static_cast<std::uint64_t>(
                        configuration.chunk_size
                    )
                }
            }
        );

        const nexusfs::storage::
            StorageRecoveryReport recovery_report =
                nexusfs::storage::
                    recover_storage_root(
                        configuration.storage_root
                    );

        metrics_registry->
            record_storage_recovery(
                recovery_report.entries_scanned,
                recovery_report
                    .temporary_entries_found,
                recovery_report
                    .temporary_files_removed,
                recovery_report
                    .non_regular_entries_preserved
            );

        logger->log(
            nexusfs::observability::
                LogLevel::info,
            "storage_recovery_completed",
            "NexusFS storage recovery completed.",
            {
                nexusfs::observability::LogField{
                    "entries_scanned",
                    recovery_report.entries_scanned
                },
                nexusfs::observability::LogField{
                    "temporary_entries_found",
                    recovery_report
                        .temporary_entries_found
                },
                nexusfs::observability::LogField{
                    "temporary_files_removed",
                    recovery_report
                        .temporary_files_removed
                },
                nexusfs::observability::LogField{
                    "non_regular_entries_preserved",
                    recovery_report
                        .non_regular_entries_preserved
                }
            }
        );

        const auto service =
            std::make_shared<
                nexusfs::app::NexusFsService
            >(
                configuration.storage_root,
                configuration.chunk_size
            );

        const nexusfs::http::HttpRouter router{
            service,
            metrics_registry,
            logger
        };

        nexusfs::http::HttpServer server{
            configuration.address,
            configuration.port,
            router
        };

        boost::asio::io_context signal_context{
            1
        };

        boost::asio::signal_set signals{
            signal_context
        };

        register_shutdown_signals(
            signals
        );

        signals.async_wait(
            [
                &server,
                logger
            ](
                const boost::system::error_code& error,
                int signal_number
            )
            {
                if (error)
                {
                    return;
                }

                logger->log(
                    nexusfs::observability::
                        LogLevel::info,
                    "daemon_shutdown_requested",
                    "NexusFS daemon received "
                    "a shutdown signal.",
                    {
                        nexusfs::observability::
                            LogField{
                                "signal_number",
                                static_cast<
                                    std::int64_t
                                >(
                                    signal_number
                                )
                            }
                    }
                );

                server.stop();
            }
        );

        std::thread signal_thread{
            [
                &signal_context
            ]()
            {
                signal_context.run();
            }
        };

        logger->log(
            nexusfs::observability::
                LogLevel::info,
            "daemon_starting",
            "NexusFS daemon is starting."
        );

        try
        {
            server.run();
        }
        catch (...)
        {
            server.stop();

            stop_signal_context(
                signal_context,
                signal_thread
            );

            logger->log(
                nexusfs::observability::
                    LogLevel::error,
                "daemon_failed",
                "NexusFS daemon terminated "
                "because of an error."
            );

            throw;
        }

        stop_signal_context(
            signal_context,
            signal_thread
        );

        logger->log(
            nexusfs::observability::
                LogLevel::info,
            "daemon_stopped",
            "NexusFS daemon stopped cleanly."
        );

        return 0;
    }
    catch (const std::invalid_argument& error)
    {
        std::cerr
            << "NexusFS daemon configuration error: "
            << error.what()
            << "\n\n"
            << nexusfs::daemon::
                daemon_usage();

        return 1;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "NexusFS daemon error: "
            << error.what()
            << '\n';

        return 1;
    }
}