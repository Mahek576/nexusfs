#include "nexusfs/security/request_security.hpp"

#include "nexusfs/storage/sha256_hasher.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nexusfs::security
{

namespace
{

constexpr std::string_view cluster_header{
    "X-NexusFS-Cluster-ID"
};

constexpr std::string_view node_header{
    "X-NexusFS-Node-ID"
};

constexpr std::string_view timestamp_header{
    "X-NexusFS-Timestamp"
};

constexpr std::string_view nonce_header{
    "X-NexusFS-Nonce"
};

constexpr std::string_view signature_header{
    "X-NexusFS-Signature"
};

constexpr std::string_view authorization_header{
    "Authorization"
};

constexpr std::string_view bearer_prefix{
    "Bearer "
};

std::string environment_value(
    const char* name
)
{
#ifdef _WIN32
    char* value =
        nullptr;

    std::size_t value_size =
        0;

    const int environment_error =
        _dupenv_s(
            &value,
            &value_size,
            name
        );

    if (environment_error != 0)
    {
        std::free(
            value
        );

        throw std::runtime_error(
            "Failed to read an environment variable."
        );
    }

    const std::string result =
        value == nullptr
        ? std::string{}
        : std::string{
              value
          };

    std::free(
        value
    );

    return result;
#else
    const char* const value =
        std::getenv(
            name
        );

    return value == nullptr
        ? std::string{}
        : std::string{
              value
          };
#endif
}

std::uint64_t unix_time_milliseconds()
{
    const auto milliseconds =
        std::chrono::duration_cast<
            std::chrono::milliseconds
        >(
            std::chrono::system_clock::now()
                .time_since_epoch()
        ).count();

    if (milliseconds <= 0)
    {
        throw std::runtime_error(
            "System clock produced an invalid timestamp."
        );
    }

    return static_cast<std::uint64_t>(
        milliseconds
    );
}

std::string lowercase_hex(
    const unsigned char* bytes,
    std::size_t size
)
{
    constexpr std::array<char, 16> digits{
        '0', '1', '2', '3',
        '4', '5', '6', '7',
        '8', '9', 'a', 'b',
        'c', 'd', 'e', 'f'
    };

    std::string result;

    result.reserve(
        size * 2
    );

    for (
        std::size_t index = 0;
        index < size;
        ++index
    )
    {
        const unsigned char byte =
            bytes[index];

        result.push_back(
            digits[
                static_cast<std::size_t>(
                    byte >> 4U
                )
            ]
        );

        result.push_back(
            digits[
                static_cast<std::size_t>(
                    byte & 0x0FU
                )
            ]
        );
    }

    return result;
}

bool is_lowercase_hex(
    std::string_view value,
    std::size_t required_size
) noexcept
{
    return (
        value.size() == required_size
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

bool constant_time_equal(
    std::string_view left,
    std::string_view right
) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }

    if (left.empty())
    {
        return true;
    }

    return (
        CRYPTO_memcmp(
            left.data(),
            right.data(),
            left.size()
        )
        == 0
    );
}

std::string body_digest(
    const RequestSecurity::Request& request
)
{
    const std::string& body =
        request.body();

    const auto* const bytes =
        reinterpret_cast<
            const std::uint8_t*
        >(
            body.data()
        );

    return storage::Sha256Hasher::hash(
        std::span<const std::uint8_t>{
            bytes,
            body.size()
        }
    );
}

std::string header_value(
    const RequestSecurity::Request& request,
    std::string_view name
)
{
    const auto field =
        request[
            boost::beast::string_view{
                name.data(),
                name.size()
            }
        ];

    return std::string{
        field.data(),
        field.size()
    };
}

std::string canonical_request(
    const RequestSecurity::Request& request,
    std::string_view cluster_id,
    std::string_view node_id,
    std::uint64_t timestamp_unix_ms,
    std::string_view nonce
)
{
    const auto method =
        request.method_string();

    const auto target =
        request.target();

    std::string canonical;

    canonical.reserve(
        method.size()
        + target.size()
        + cluster_id.size()
        + node_id.size()
        + nonce.size()
        + 128
    );

    canonical.append(
        method.data(),
        method.size()
    );

    canonical.push_back('\n');

    canonical.append(
        target.data(),
        target.size()
    );

    canonical.push_back('\n');

    canonical.append(
        cluster_id.data(),
        cluster_id.size()
    );

    canonical.push_back('\n');

    canonical.append(
        node_id.data(),
        node_id.size()
    );

    canonical.push_back('\n');

    canonical +=
        std::to_string(
            timestamp_unix_ms
        );

    canonical.push_back('\n');

    canonical.append(
        nonce.data(),
        nonce.size()
    );

    canonical.push_back('\n');

    canonical +=
        body_digest(
            request
        );

    return canonical;
}

std::string hmac_sha256(
    std::string_view secret,
    std::string_view message
)
{
    if (secret.empty())
    {
        throw std::invalid_argument(
            "HMAC secret cannot be empty."
        );
    }

    if (
        secret.size() >
        static_cast<std::size_t>(
            INT_MAX
        )
    )
    {
        throw std::overflow_error(
            "HMAC secret exceeds the supported size."
        );
    }

    std::array<
        unsigned char,
        EVP_MAX_MD_SIZE
    > digest{};

    unsigned int digest_size =
        0;

    const unsigned char* const result =
        HMAC(
            EVP_sha256(),
            secret.data(),
            static_cast<int>(
                secret.size()
            ),
            reinterpret_cast<
                const unsigned char*
            >(
                message.data()
            ),
            message.size(),
            digest.data(),
            &digest_size
        );

    if (
        result == nullptr
        || digest_size != 32U
    )
    {
        throw std::runtime_error(
            "HMAC-SHA256 signing failed."
        );
    }

    return lowercase_hex(
        digest.data(),
        digest_size
    );
}

bool timestamp_within_window(
    std::uint64_t supplied,
    std::uint64_t current,
    std::uint64_t maximum_skew
) noexcept
{
    if (supplied >= current)
    {
        return (
            supplied - current
            <= maximum_skew
        );
    }

    return (
        current - supplied
        <= maximum_skew
    );
}

AuthenticationResult rejected(
    AuthenticationStatus status,
    std::string reason,
    bool replayed = false
)
{
    return AuthenticationResult{
        status,
        false,
        replayed,
        std::move(reason)
    };
}

}

struct RequestSecurity::State
{
    State(
        std::string cluster_secret_value,
        std::string admin_token_value,
        std::chrono::milliseconds maximum_clock_skew
    )
        : cluster_secret{
              std::move(
                  cluster_secret_value
              )
          },
          admin_token{
              std::move(
                  admin_token_value
              )
          },
          maximum_clock_skew_ms{
              static_cast<std::uint64_t>(
                  maximum_clock_skew.count()
              )
          }
    {
    }

    std::string cluster_secret;
    std::string admin_token;

    std::uint64_t maximum_clock_skew_ms{
        60000
    };

    mutable std::mutex replay_mutex;

    mutable std::unordered_map<
        std::string,
        std::uint64_t
    > accepted_nonces;
};

std::string_view authentication_status_name(
    AuthenticationStatus status
) noexcept
{
    switch (status)
    {
        case AuthenticationStatus::accepted:
            return "accepted";

        case AuthenticationStatus::security_disabled:
            return "security_disabled";

        case AuthenticationStatus::admin_disabled:
            return "admin_disabled";

        case AuthenticationStatus::missing_credentials:
            return "missing_credentials";

        case AuthenticationStatus::invalid_cluster:
            return "invalid_cluster";

        case AuthenticationStatus::unauthorized_node:
            return "unauthorized_node";

        case AuthenticationStatus::invalid_timestamp:
            return "invalid_timestamp";

        case AuthenticationStatus::expired_timestamp:
            return "expired_timestamp";

        case AuthenticationStatus::invalid_nonce:
            return "invalid_nonce";

        case AuthenticationStatus::invalid_signature:
            return "invalid_signature";

        case AuthenticationStatus::replayed:
            return "replayed";
    }

    return "missing_credentials";
}

RequestSecurity::RequestSecurity()
    : RequestSecurity{
          environment_value(
              "NEXUSFS_CLUSTER_SECRET"
          ),
          environment_value(
              "NEXUSFS_ADMIN_TOKEN"
          )
      }
{
}

RequestSecurity::RequestSecurity(
    std::string cluster_secret,
    std::string admin_token,
    std::chrono::milliseconds maximum_clock_skew
)
    : state_{
          std::make_shared<State>(
              std::move(
                  cluster_secret
              ),
              std::move(
                  admin_token
              ),
              maximum_clock_skew
          )
      }
{
    if (
        maximum_clock_skew <=
        std::chrono::milliseconds::zero()
    )
    {
        throw std::invalid_argument(
            "Maximum authentication clock skew "
            "must be positive."
        );
    }

    if (
        !state_->cluster_secret.empty()
        && state_->cluster_secret.size() < 32
    )
    {
        throw std::invalid_argument(
            "NEXUSFS_CLUSTER_SECRET must contain "
            "at least 32 characters."
        );
    }

    if (
        !state_->admin_token.empty()
        && state_->admin_token.size() < 16
    )
    {
        throw std::invalid_argument(
            "NEXUSFS_ADMIN_TOKEN must contain "
            "at least 16 characters."
        );
    }
}

RequestSecurity::~RequestSecurity() = default;

bool RequestSecurity::peer_signing_enabled()
    const noexcept
{
    return !state_->cluster_secret.empty();
}

bool RequestSecurity::admin_authentication_enabled()
    const noexcept
{
    return !state_->admin_token.empty();
}

std::string RequestSecurity::generate_nonce()
{
    std::array<unsigned char, 16> nonce{};

    if (
        RAND_bytes(
            nonce.data(),
            static_cast<int>(
                nonce.size()
            )
        )
        != 1
    )
    {
        throw std::runtime_error(
            "Secure nonce generation failed."
        );
    }

    return lowercase_hex(
        nonce.data(),
        nonce.size()
    );
}

void RequestSecurity::sign_peer_request(
    Request& request,
    std::string_view cluster_id,
    std::string_view node_id
) const
{
    sign_peer_request(
        request,
        cluster_id,
        node_id,
        unix_time_milliseconds(),
        generate_nonce()
    );
}

void RequestSecurity::sign_peer_request(
    Request& request,
    std::string_view cluster_id,
    std::string_view node_id,
    std::uint64_t timestamp_unix_ms,
    std::string nonce
) const
{
    if (!peer_signing_enabled())
    {
        return;
    }

    if (
        cluster_id.empty()
        || node_id.empty()
        || timestamp_unix_ms == 0
        || !is_lowercase_hex(
            nonce,
            32
        )
    )
    {
        throw std::invalid_argument(
            "Peer signing metadata is invalid."
        );
    }

    request.set(
        cluster_header,
        cluster_id
    );

    request.set(
        node_header,
        node_id
    );

    request.set(
        timestamp_header,
        std::to_string(
            timestamp_unix_ms
        )
    );

    request.set(
        nonce_header,
        nonce
    );

    const std::string canonical =
        canonical_request(
            request,
            cluster_id,
            node_id,
            timestamp_unix_ms,
            nonce
        );

    request.set(
        signature_header,
        hmac_sha256(
            state_->cluster_secret,
            canonical
        )
    );
}

AuthenticationResult
RequestSecurity::verify_peer_request(
    const Request& request,
    const cluster::ClusterNodeFoundation& cluster_node,
    bool allow_unknown_node
) const
{
    if (!peer_signing_enabled())
    {
        return AuthenticationResult{
            AuthenticationStatus::security_disabled,
            true,
            false,
            "Peer request signing is disabled."
        };
    }

    const std::string cluster_id =
        header_value(
            request,
            cluster_header
        );

    const std::string node_id =
        header_value(
            request,
            node_header
        );

    const std::string timestamp_text =
        header_value(
            request,
            timestamp_header
        );

    const std::string nonce =
        header_value(
            request,
            nonce_header
        );

    const std::string signature =
        header_value(
            request,
            signature_header
        );

    if (
        cluster_id.empty()
        || node_id.empty()
        || timestamp_text.empty()
        || nonce.empty()
        || signature.empty()
    )
    {
        return rejected(
            AuthenticationStatus::missing_credentials,
            "Required signed peer headers are missing."
        );
    }

    if (
        cluster_id !=
        cluster_node
            .configuration()
            .cluster_id
    )
    {
        return rejected(
            AuthenticationStatus::invalid_cluster,
            "The signed request belongs to another cluster."
        );
    }

    const bool local_node =
        node_id ==
        cluster_node
            .identity()
            .node_id;

    if (
        !local_node
        && !allow_unknown_node
        && !cluster_node.is_known_peer(
            node_id
        )
    )
    {
        return rejected(
            AuthenticationStatus::unauthorized_node,
            "The signed node is not an authorized peer."
        );
    }

    std::uint64_t timestamp =
        0;

    const char* const timestamp_begin =
        timestamp_text.data();

    const char* const timestamp_end =
        timestamp_begin
        + timestamp_text.size();

    const auto [timestamp_position, timestamp_error] =
        std::from_chars(
            timestamp_begin,
            timestamp_end,
            timestamp
        );

    if (
        timestamp_error != std::errc{}
        || timestamp_position != timestamp_end
        || timestamp == 0
    )
    {
        return rejected(
            AuthenticationStatus::invalid_timestamp,
            "The peer timestamp is invalid."
        );
    }

    const std::uint64_t now =
        unix_time_milliseconds();

    if (
        !timestamp_within_window(
            timestamp,
            now,
            state_->maximum_clock_skew_ms
        )
    )
    {
        return rejected(
            AuthenticationStatus::expired_timestamp,
            "The signed peer request is outside "
            "the accepted clock window."
        );
    }

    if (
        !is_lowercase_hex(
            nonce,
            32
        )
    )
    {
        return rejected(
            AuthenticationStatus::invalid_nonce,
            "The peer nonce is invalid."
        );
    }

    if (
        !is_lowercase_hex(
            signature,
            64
        )
    )
    {
        return rejected(
            AuthenticationStatus::invalid_signature,
            "The peer signature is malformed."
        );
    }

    const std::string canonical =
        canonical_request(
            request,
            cluster_id,
            node_id,
            timestamp,
            nonce
        );

    const std::string expected_signature =
        hmac_sha256(
            state_->cluster_secret,
            canonical
        );

    if (
        !constant_time_equal(
            signature,
            expected_signature
        )
    )
    {
        return rejected(
            AuthenticationStatus::invalid_signature,
            "The peer request signature is invalid."
        );
    }

    const std::string replay_key =
        node_id
        + ":"
        + nonce;

    {
        const std::lock_guard lock{
            state_->replay_mutex
        };

        const std::uint64_t retention =
            state_->maximum_clock_skew_ms
            <= (
                std::numeric_limits<
                    std::uint64_t
                >::max()
                / 2
            )
            ? state_->maximum_clock_skew_ms * 2
            : std::numeric_limits<
                  std::uint64_t
              >::max();

        for (
            auto iterator =
                state_->accepted_nonces.begin();
            iterator !=
                state_->accepted_nonces.end();
        )
        {
            if (
                now >= iterator->second
                && now - iterator->second > retention
            )
            {
                iterator =
                    state_->accepted_nonces.erase(
                        iterator
                    );
            }
            else
            {
                ++iterator;
            }
        }

        if (
            state_->accepted_nonces.contains(
                replay_key
            )
        )
        {
            return rejected(
                AuthenticationStatus::replayed,
                "The signed peer nonce has already "
                "been accepted.",
                true
            );
        }

        state_->accepted_nonces.emplace(
            replay_key,
            now
        );
    }

    return AuthenticationResult{
        AuthenticationStatus::accepted,
        true,
        false,
        "Signed peer request accepted."
    };
}

AuthenticationResult
RequestSecurity::verify_admin_request(
    const Request& request
) const
{
    if (!admin_authentication_enabled())
    {
        return rejected(
            AuthenticationStatus::admin_disabled,
            "Administrative authentication is not configured."
        );
    }

    const std::string authorization =
        header_value(
            request,
            authorization_header
        );

    if (
        !authorization.starts_with(
            bearer_prefix
        )
    )
    {
        return rejected(
            AuthenticationStatus::missing_credentials,
            "The administrative bearer token is missing."
        );
    }

    const std::string_view supplied_token{
        authorization.data()
            + bearer_prefix.size(),
        authorization.size()
            - bearer_prefix.size()
    };

    if (
        !constant_time_equal(
            supplied_token,
            state_->admin_token
        )
    )
    {
        return rejected(
            AuthenticationStatus::invalid_signature,
            "The administrative bearer token is invalid."
        );
    }

    return AuthenticationResult{
        AuthenticationStatus::accepted,
        true,
        false,
        "Administrative request accepted."
    };
}

}
