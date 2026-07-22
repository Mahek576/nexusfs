#include "nexusfs/admin/admin_client.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace nexusfs::admin
{

namespace
{

namespace asio =
    boost::asio;

namespace beast =
    boost::beast;

namespace beast_http =
    boost::beast::http;

using Tcp =
    asio::ip::tcp;

}

AdminClient::AdminClient(
    std::string address,
    std::uint16_t port,
    std::string bearer_token,
    std::chrono::milliseconds timeout
)
    : address_{
          std::move(address)
      },
      port_{
          port
      },
      bearer_token_{
          std::move(bearer_token)
      },
      timeout_{
          timeout
      }
{
    if (address_.empty())
    {
        throw std::invalid_argument(
            "Admin client address cannot be empty."
        );
    }

    if (port_ == 0)
    {
        throw std::invalid_argument(
            "Admin client port cannot be zero."
        );
    }

    if (bearer_token_.empty())
    {
        throw std::invalid_argument(
            "Admin client bearer token cannot be empty."
        );
    }

    if (
        timeout_ <=
        std::chrono::milliseconds::zero()
    )
    {
        throw std::invalid_argument(
            "Admin client timeout must be positive."
        );
    }
}

AdminResponse AdminClient::get(
    std::string target
) const
{
    return request(
        "GET",
        std::move(target),
        {}
    );
}

AdminResponse AdminClient::post(
    std::string target,
    std::string json_body
) const
{
    return request(
        "POST",
        std::move(target),
        std::move(json_body)
    );
}

AdminResponse AdminClient::request(
    std::string method,
    std::string target,
    std::string body
) const
{
    asio::io_context context{
        1
    };

    boost::system::error_code
        address_error;

    const asio::ip::address address =
        asio::ip::make_address(
            address_,
            address_error
        );

    if (address_error)
    {
        throw std::runtime_error(
            "Admin server address is invalid: "
            + address_error.message()
        );
    }

    beast::tcp_stream stream{
        context
    };

    stream.expires_after(
        timeout_
    );

    stream.connect(
        Tcp::endpoint{
            address,
            port_
        }
    );

    const beast_http::verb verb =
        method == "POST"
        ? beast_http::verb::post
        : beast_http::verb::get;

    beast_http::request<
        beast_http::string_body
    > request_message{
        verb,
        std::move(target),
        11
    };

    request_message.set(
        beast_http::field::host,
        address_
    );

    request_message.set(
        beast_http::field::user_agent,
        "NexusFS-AdminClient/1"
    );

    request_message.set(
        beast_http::field::authorization,
        "Bearer "
            + bearer_token_
    );

    request_message.set(
        beast_http::field::accept,
        "application/json"
    );

    if (verb == beast_http::verb::post)
    {
        request_message.set(
            beast_http::field::content_type,
            "application/json"
        );

        request_message.body() =
            std::move(body);
    }

    request_message.keep_alive(
        false
    );

    request_message.prepare_payload();

    stream.expires_after(
        timeout_
    );

    beast_http::write(
        stream,
        request_message
    );

    beast::flat_buffer buffer;

    beast_http::response<
        beast_http::string_body
    > response;

    stream.expires_after(
        timeout_
    );

    beast_http::read(
        stream,
        buffer,
        response
    );

    boost::system::error_code
        shutdown_error;

    stream.socket().shutdown(
        Tcp::socket::shutdown_both,
        shutdown_error
    );

    return AdminResponse{
        static_cast<unsigned int>(
            response.result_int()
        ),
        std::move(
            response.body()
        )
    };
}

}
