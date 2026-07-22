#ifndef NEXUSFS_SECURITY_REQUEST_SECURITY_HPP
#define NEXUSFS_SECURITY_REQUEST_SECURITY_HPP

#include "nexusfs/cluster/cluster_node_foundation.hpp"

#include <boost/beast/http.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace nexusfs::security
{

enum class AuthenticationStatus
{
    accepted,
    security_disabled,
    admin_disabled,
    missing_credentials,
    invalid_cluster,
    unauthorized_node,
    invalid_timestamp,
    expired_timestamp,
    invalid_nonce,
    invalid_signature,
    replayed
};

[[nodiscard]] std::string_view
authentication_status_name(
    AuthenticationStatus status
) noexcept;

struct AuthenticationResult
{
    AuthenticationStatus status{
        AuthenticationStatus::missing_credentials
    };

    bool accepted{false};
    bool replayed{false};

    std::string reason;
};

/*
 * Application-layer request authentication.
 *
 * Peer requests use HMAC-SHA256 over a canonical request containing:
 *
 *   method
 *   target
 *   cluster ID
 *   node ID
 *   timestamp
 *   nonce
 *   SHA-256 body digest
 *
 * Admin requests use a bearer token compared in constant time.
 *
 * Configuration is read from:
 *
 *   NEXUSFS_CLUSTER_SECRET
 *   NEXUSFS_ADMIN_TOKEN
 *
 * Empty variables disable the corresponding security boundary so
 * existing embedded and test deployments remain backwards compatible.
 */
class RequestSecurity final
{
public:
    using Request =
        boost::beast::http::request<
            boost::beast::http::string_body
        >;

    RequestSecurity();

    RequestSecurity(
        std::string cluster_secret,
        std::string admin_token,
        std::chrono::milliseconds maximum_clock_skew =
            std::chrono::seconds{
                60
            }
    );

    ~RequestSecurity();

    RequestSecurity(
        const RequestSecurity&
    ) = default;

    RequestSecurity& operator=(
        const RequestSecurity&
    ) = default;

    [[nodiscard]] bool
    peer_signing_enabled() const noexcept;

    [[nodiscard]] bool
    admin_authentication_enabled() const noexcept;

    void sign_peer_request(
        Request& request,
        std::string_view cluster_id,
        std::string_view node_id
    ) const;

    /*
     * Explicit timestamp and nonce overload used by deterministic
     * validation and fault-injection tests.
     */
    void sign_peer_request(
        Request& request,
        std::string_view cluster_id,
        std::string_view node_id,
        std::uint64_t timestamp_unix_ms,
        std::string nonce
    ) const;

    [[nodiscard]] AuthenticationResult
    verify_peer_request(
        const Request& request,
        const cluster::ClusterNodeFoundation& cluster_node,
        bool allow_unknown_node = false
    ) const;

    [[nodiscard]] AuthenticationResult
    verify_admin_request(
        const Request& request
    ) const;

    [[nodiscard]] static std::string
    generate_nonce();

private:
    struct State;

    std::shared_ptr<State> state_;
};

}

#endif
