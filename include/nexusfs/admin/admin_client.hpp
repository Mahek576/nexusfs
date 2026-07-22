#ifndef NEXUSFS_ADMIN_ADMIN_CLIENT_HPP
#define NEXUSFS_ADMIN_ADMIN_CLIENT_HPP

#include <chrono>
#include <cstdint>
#include <string>

namespace nexusfs::admin
{

struct AdminResponse
{
    unsigned int status_code{0};
    std::string body;

    [[nodiscard]] bool successful() const noexcept
    {
        return (
            status_code >= 200U
            && status_code < 300U
        );
    }
};

class AdminClient final
{
public:
    AdminClient(
        std::string address,
        std::uint16_t port,
        std::string bearer_token,
        std::chrono::milliseconds timeout =
            std::chrono::milliseconds{
                5000
            }
    );

    [[nodiscard]] AdminResponse get(
        std::string target
    ) const;

    [[nodiscard]] AdminResponse post(
        std::string target,
        std::string json_body = "{}"
    ) const;

private:
    [[nodiscard]] AdminResponse request(
        std::string method,
        std::string target,
        std::string body
    ) const;

    std::string address_;
    std::uint16_t port_;
    std::string bearer_token_;
    std::chrono::milliseconds timeout_;
};

}

#endif
