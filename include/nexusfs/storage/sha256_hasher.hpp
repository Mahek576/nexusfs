#ifndef NEXUSFS_STORAGE_SHA256_HASHER_HPP
#define NEXUSFS_STORAGE_SHA256_HASHER_HPP

#include <cstdint>
#include <span>
#include <string>

namespace nexusfs::storage
{

class Sha256Hasher
{
public:
    [[nodiscard]] static std::string hash(
        std::span<const std::uint8_t> data
    );
};

}

#endif