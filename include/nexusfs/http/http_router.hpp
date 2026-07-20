#ifndef NEXUSFS_HTTP_HTTP_ROUTER_HPP
#define NEXUSFS_HTTP_HTTP_ROUTER_HPP

#include "nexusfs/app/nexusfs_service.hpp"

#include <boost/beast/http.hpp>

#include <memory>

namespace nexusfs::http
{

class HttpRouter
{
public:
    using Request =
        boost::beast::http::request<
            boost::beast::http::string_body
        >;

    using Response =
        boost::beast::http::response<
            boost::beast::http::string_body
        >;

    explicit HttpRouter(
        std::shared_ptr<
            const app::NexusFsService
        > service
    );

    [[nodiscard]] Response route(
        const Request& request
    ) const;

private:
    std::shared_ptr<
        const app::NexusFsService
    > service_;
};

}

#endif