#ifndef NEXUSFS_HTTP_HTTP_SERVER_HPP
#define NEXUSFS_HTTP_HTTP_SERVER_HPP

#include "nexusfs/http/http_router.hpp"

#include <cstdint>
#include <string>

namespace nexusfs::http
{

class HttpServer
{
public:
    HttpServer(
        std::string address,
        std::uint16_t port,
        HttpRouter router
    );

    void run() const;

    [[nodiscard]] const std::string&
    address() const noexcept;

    [[nodiscard]] std::uint16_t
    port() const noexcept;

private:
    std::string address_;
    std::uint16_t port_;
    HttpRouter router_;
};

}

#endif