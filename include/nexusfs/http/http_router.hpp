#ifndef NEXUSFS_HTTP_HTTP_ROUTER_HPP
#define NEXUSFS_HTTP_HTTP_ROUTER_HPP

#include <boost/beast/http.hpp>

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

    [[nodiscard]] Response route(
        const Request& request
    ) const;
};

}

#endif