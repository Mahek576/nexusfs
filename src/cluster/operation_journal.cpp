#include "nexusfs/cluster/operation_journal.hpp"

#include "nexusfs/storage/durable_file.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace nexusfs::cluster
{

namespace
{

void validate_operation_id(
    std::string_view operation_id
)
{
    if (
        operation_id.empty()
        || operation_id.size() > 128
    )
    {
        throw std::invalid_argument(
            "Operation ID must contain between "
            "1 and 128 characters."
        );
    }

    const bool valid =
        std::all_of(
            operation_id.begin(),
            operation_id.end(),
            [](char character)
            {
                return (
                    (
                        character >= 'a'
                        && character <= 'z'
                    )
                    || (
                        character >= 'A'
                        && character <= 'Z'
                    )
                    || (
                        character >= '0'
                        && character <= '9'
                    )
                    || character == '-'
                    || character == '_'
                    || character == '.'
                );
            }
        );

    if (!valid)
    {
        throw std::invalid_argument(
            "Operation ID may contain only letters, "
            "numbers, hyphens, underscores and periods."
        );
    }
}

bool is_lowercase_sha256(
    std::string_view value
) noexcept
{
    return (
        value.size() == 64
        && std::all_of(
            value.begin(),
            value.end(),
            [](char character)
            {
                return (
                    (
                        character >= '0'
                        && character <= '9'
                    )
                    || (
                        character >= 'a'
                        && character <= 'f'
                    )
                );
            }
        )
    );
}

std::string operation_filename(
    std::string_view operation_id
)
{
    const std::vector<std::uint8_t> bytes{
        operation_id.begin(),
        operation_id.end()
    };

    return (
        storage::Sha256Hasher::hash(
            std::span<const std::uint8_t>{
                bytes.data(),
                bytes.size()
            }
        )
        + ".json"
    );
}

std::string read_text_file(
    const std::filesystem::path& path
)
{
    std::ifstream input{
        path,
        std::ios::binary
    };

    if (!input.is_open())
    {
        throw std::runtime_error(
            "Failed to open operation journal record: "
            + path.string()
        );
    }

    return std::string{
        std::istreambuf_iterator<char>{
            input
        },
        std::istreambuf_iterator<char>{}
    };
}

RebalanceOperationRecord decode_record(
    const std::filesystem::path& path
)
{
    try
    {
        const nlohmann::json payload =
            nlohmann::json::parse(
                read_text_file(
                    path
                )
            );

        if (
            !payload.is_object()
            || payload.at(
                "schema_version"
            ).get<std::uint64_t>() != 1
            || payload.at(
                "operation_kind"
            ).get<std::string>() !=
                "rebalance"
        )
        {
            throw std::runtime_error(
                "Unsupported operation journal schema."
            );
        }

        RebalanceOperationRecord record{
            payload.at(
                "operation_id"
            ).get<std::string>(),
            payload.at(
                "request_digest"
            ).get<std::string>(),
            payload.at(
                "membership_epoch"
            ).get<std::uint64_t>(),
            payload.at(
                "replication_factor"
            ).get<std::uint64_t>(),
            payload.at(
                "chunks_scanned"
            ).get<std::uint64_t>(),
            payload.at(
                "targets_planned"
            ).get<std::uint64_t>(),
            payload.at(
                "replicas_observed"
            ).get<std::uint64_t>(),
            payload.at(
                "replicas_created"
            ).get<std::uint64_t>(),
            payload.at(
                "peer_failures"
            ).get<std::uint64_t>(),
            payload.at(
                "under_replicated_chunks"
            ).get<std::uint64_t>(),
            payload.at(
                "converged"
            ).get<bool>()
        };

        validate_operation_id(
            record.operation_id
        );

        if (
            !is_lowercase_sha256(
                record.request_digest
            )
            || record.membership_epoch == 0
            || record.replication_factor == 0
        )
        {
            throw std::runtime_error(
                "Operation journal record contains "
                "invalid consistency metadata."
            );
        }

        return record;
    }
    catch (const OperationIdConflict&)
    {
        throw;
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(
            std::string{
                "Failed to decode operation journal record: "
            }
            + error.what()
        );
    }
}

nlohmann::ordered_json encode_record(
    const RebalanceOperationRecord& record
)
{
    return {
        {
            "schema_version",
            1
        },
        {
            "operation_kind",
            "rebalance"
        },
        {
            "operation_id",
            record.operation_id
        },
        {
            "request_digest",
            record.request_digest
        },
        {
            "membership_epoch",
            record.membership_epoch
        },
        {
            "replication_factor",
            record.replication_factor
        },
        {
            "chunks_scanned",
            record.chunks_scanned
        },
        {
            "targets_planned",
            record.targets_planned
        },
        {
            "replicas_observed",
            record.replicas_observed
        },
        {
            "replicas_created",
            record.replicas_created
        },
        {
            "peer_failures",
            record.peer_failures
        },
        {
            "under_replicated_chunks",
            record.under_replicated_chunks
        },
        {
            "converged",
            record.converged
        }
    };
}

void remove_temporary_file(
    const std::filesystem::path& path
) noexcept
{
    std::error_code removal_error;

    std::filesystem::remove(
        path,
        removal_error
    );
}

}

OperationJournal::OperationJournal(
    const std::filesystem::path& cluster_directory
)
    : rebalance_directory_{
          cluster_directory
          / "operations"
          / "rebalance"
      }
{
    if (cluster_directory.empty())
    {
        throw std::invalid_argument(
            "Operation journal cluster directory "
            "cannot be empty."
        );
    }

    std::error_code directory_error;

    std::filesystem::create_directories(
        rebalance_directory_,
        directory_error
    );

    if (directory_error)
    {
        throw std::runtime_error(
            "Failed to create operation journal directory: "
            + directory_error.message()
        );
    }
}

std::optional<RebalanceOperationRecord>
OperationJournal::load_rebalance(
    std::string_view operation_id
) const
{
    validate_operation_id(
        operation_id
    );

    const std::filesystem::path path =
        rebalance_directory_
        / operation_filename(
            operation_id
        );

    std::error_code existence_error;

    const bool exists =
        std::filesystem::exists(
            path,
            existence_error
        );

    if (existence_error)
    {
        throw std::runtime_error(
            "Failed to inspect operation journal: "
            + existence_error.message()
        );
    }

    if (!exists)
    {
        return std::nullopt;
    }

    std::error_code type_error;

    if (
        !std::filesystem::is_regular_file(
            path,
            type_error
        )
        || type_error
    )
    {
        throw std::runtime_error(
            "Operation journal destination is not "
            "a regular file."
        );
    }

    RebalanceOperationRecord record =
        decode_record(
            path
        );

    if (record.operation_id != operation_id)
    {
        throw OperationIdConflict(
            "Operation journal key collision detected."
        );
    }

    return record;
}

RebalanceOperationRecord
OperationJournal::publish_rebalance(
    const RebalanceOperationRecord& record
)
{
    validate_operation_id(
        record.operation_id
    );

    if (
        !is_lowercase_sha256(
            record.request_digest
        )
        || record.membership_epoch == 0
        || record.replication_factor == 0
    )
    {
        throw std::invalid_argument(
            "Completed rebalance record contains "
            "invalid consistency metadata."
        );
    }

    if (
        const auto existing =
            load_rebalance(
                record.operation_id
            )
    )
    {
        if (
            existing->request_digest !=
            record.request_digest
        )
        {
            throw OperationIdConflict(
                "Operation ID has already been used "
                "for a different rebalance request."
            );
        }

        return *existing;
    }

    static std::atomic<std::uint64_t>
        temporary_sequence{
            0
        };

    const std::filesystem::path destination =
        rebalance_directory_
        / operation_filename(
            record.operation_id
        );

    const std::filesystem::path temporary =
        rebalance_directory_
        / (
            destination.filename().string()
            + ".tmp."
            + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count()
            )
            + "."
            + std::to_string(
                temporary_sequence.fetch_add(
                    1,
                    std::memory_order_relaxed
                )
            )
        );

    const std::string encoded =
        encode_record(
            record
        ).dump()
        + "\n";

    {
        std::ofstream output{
            temporary,
            std::ios::binary
                | std::ios::trunc
        };

        if (!output.is_open())
        {
            throw std::runtime_error(
                "Failed to create temporary operation "
                "journal record."
            );
        }

        output.write(
            encoded.data(),
            static_cast<std::streamsize>(
                encoded.size()
            )
        );

        output.close();

        if (!output)
        {
            remove_temporary_file(
                temporary
            );

            throw std::runtime_error(
                "Failed to write temporary operation "
                "journal record."
            );
        }
    }

    try
    {
        const storage::DurablePublishResult result =
            storage::publish_file_durably(
                temporary,
                destination
            );

        if (
            result ==
            storage::DurablePublishResult::published
        )
        {
            return record;
        }

        remove_temporary_file(
            temporary
        );

        const auto existing =
            load_rebalance(
                record.operation_id
            );

        if (!existing)
        {
            throw std::runtime_error(
                "Operation journal publication reported "
                "an existing destination that could not "
                "be loaded."
            );
        }

        if (
            existing->request_digest !=
            record.request_digest
        )
        {
            throw OperationIdConflict(
                "Operation ID was concurrently committed "
                "for a different request."
            );
        }

        return *existing;
    }
    catch (...)
    {
        remove_temporary_file(
            temporary
        );

        throw;
    }
}

}
