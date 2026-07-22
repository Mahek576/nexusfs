#ifndef NEXUSFS_CLUSTER_OPERATION_JOURNAL_HPP
#define NEXUSFS_CLUSTER_OPERATION_JOURNAL_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nexusfs::cluster
{

class OperationIdConflict final
    : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct RebalanceOperationRecord
{
    std::string operation_id;
    std::string request_digest;

    std::uint64_t membership_epoch{0};
    std::uint64_t replication_factor{0};

    std::uint64_t chunks_scanned{0};
    std::uint64_t targets_planned{0};

    std::uint64_t replicas_observed{0};
    std::uint64_t replicas_created{0};

    std::uint64_t peer_failures{0};
    std::uint64_t under_replicated_chunks{0};

    bool converged{false};

    [[nodiscard]] bool operator==(
        const RebalanceOperationRecord&
    ) const = default;
};

/*
 * Durable completed-operation ledger.
 *
 * Operation IDs map to immutable, no-replace JSON records. Reusing
 * an ID with the same request returns the persisted result. Reusing
 * an ID with a different request digest is rejected.
 */
class OperationJournal final
{
public:
    explicit OperationJournal(
        const std::filesystem::path& cluster_directory
    );

    [[nodiscard]] std::optional<
        RebalanceOperationRecord
    > load_rebalance(
        std::string_view operation_id
    ) const;

    [[nodiscard]] RebalanceOperationRecord
    publish_rebalance(
        const RebalanceOperationRecord& record
    );

private:
    std::filesystem::path
        rebalance_directory_;
};

}

#endif
