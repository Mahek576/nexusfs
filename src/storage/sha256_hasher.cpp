#include "nexusfs/storage/sha256_hasher.hpp"

#include <array>
#include <iomanip>
#include <memory>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>

namespace nexusfs::storage
{

namespace
{

struct EvpContextDeleter
{
    void operator()(EVP_MD_CTX* context) const noexcept
    {
        EVP_MD_CTX_free(context);
    }
};

}

std::string Sha256Hasher::hash(
    std::span<const std::uint8_t> data
)
{
    std::unique_ptr<EVP_MD_CTX, EvpContextDeleter> context{
        EVP_MD_CTX_new()
    };

    if (!context)
    {
        throw std::runtime_error(
            "Failed to create OpenSSL digest context."
        );
    }

    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    {
        throw std::runtime_error(
            "Failed to initialize SHA-256 hashing."
        );
    }

    if (!data.empty())
    {
        if (EVP_DigestUpdate(
                context.get(),
                data.data(),
                data.size()
            ) != 1)
        {
            throw std::runtime_error(
                "Failed to process data for SHA-256 hashing."
            );
        }
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;

    if (EVP_DigestFinal_ex(
            context.get(),
            digest.data(),
            &digest_length
        ) != 1)
    {
        throw std::runtime_error(
            "Failed to finalize SHA-256 hashing."
        );
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');

    for (unsigned int index = 0; index < digest_length; ++index)
    {
        output << std::setw(2)
               << static_cast<unsigned int>(digest[index]);
    }

    return output.str();
}

}