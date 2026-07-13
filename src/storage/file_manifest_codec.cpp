#include "nexusfs/storage/file_manifest_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nexusfs::storage
{

namespace
{

constexpr std::array<std::uint8_t, 8> manifest_magic{
    static_cast<std::uint8_t>('N'),
    static_cast<std::uint8_t>('E'),
    static_cast<std::uint8_t>('X'),
    static_cast<std::uint8_t>('U'),
    static_cast<std::uint8_t>('S'),
    static_cast<std::uint8_t>('F'),
    static_cast<std::uint8_t>('S'),
    static_cast<std::uint8_t>('M')
};

constexpr std::size_t encoded_hash_size = 64;

class ByteReader
{
public:
    explicit ByteReader(
        std::span<const std::uint8_t> input
    )
        : input_{input}
    {
    }

    [[nodiscard]] std::uint8_t read_byte()
    {
        ensure_available(1);

        const std::uint8_t value =
            input_[position_];

        ++position_;

        return value;
    }

    [[nodiscard]] std::uint32_t
    read_uint32_big_endian()
    {
        std::uint32_t value = 0;

        for (std::size_t index = 0; index < 4; ++index)
        {
            value =
                static_cast<std::uint32_t>(
                    (value << 8U) |
                    static_cast<std::uint32_t>(
                        read_byte()
                    )
                );
        }

        return value;
    }

    [[nodiscard]] std::uint64_t
    read_uint64_big_endian()
    {
        std::uint64_t value = 0;

        for (std::size_t index = 0; index < 8; ++index)
        {
            value =
                static_cast<std::uint64_t>(
                    (value << 8U) |
                    static_cast<std::uint64_t>(
                        read_byte()
                    )
                );
        }

        return value;
    }

    [[nodiscard]] std::string read_text(
        std::size_t length
    )
    {
        ensure_available(length);

        std::string text;
        text.reserve(length);

        for (std::size_t index = 0; index < length; ++index)
        {
            text.push_back(
                static_cast<char>(
                    input_[position_ + index]
                )
            );
        }

        position_ += length;

        return text;
    }

    [[nodiscard]] std::size_t
    remaining() const noexcept
    {
        return input_.size() - position_;
    }

    [[nodiscard]] bool
    at_end() const noexcept
    {
        return position_ == input_.size();
    }

private:
    void ensure_available(
        std::size_t requested_size
    ) const
    {
        if (requested_size > remaining())
        {
            throw std::runtime_error(
                "Encoded manifest is truncated."
            );
        }
    }

    std::span<const std::uint8_t> input_;
    std::size_t position_ = 0;
};

void append_uint32_big_endian(
    std::vector<std::uint8_t>& output,
    std::uint32_t value
)
{
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        output.push_back(
            static_cast<std::uint8_t>(
                (value >> shift) & 0xFFU
            )
        );
    }
}

void append_uint64_big_endian(
    std::vector<std::uint8_t>& output,
    std::uint64_t value
)
{
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        output.push_back(
            static_cast<std::uint8_t>(
                (value >> shift) & 0xFFULL
            )
        );
    }
}

void append_text(
    std::vector<std::uint8_t>& output,
    const std::string& text
)
{
    for (const unsigned char character : text)
    {
        output.push_back(
            static_cast<std::uint8_t>(character)
        );
    }
}

std::size_t checked_add(
    std::size_t left,
    std::size_t right
)
{
    if (
        right >
        std::numeric_limits<std::size_t>::max() - left
    )
    {
        throw std::overflow_error(
            "Encoded manifest size exceeds the platform limit."
        );
    }

    return left + right;
}

std::size_t checked_multiply(
    std::size_t left,
    std::size_t right
)
{
    if (
        left != 0 &&
        right >
        std::numeric_limits<std::size_t>::max() / left
    )
    {
        throw std::overflow_error(
            "Encoded manifest size exceeds the platform limit."
        );
    }

    return left * right;
}

std::size_t convert_uint64_to_size(
    std::uint64_t value,
    const char* error_message
)
{
    if constexpr (
        sizeof(std::size_t) <
        sizeof(std::uint64_t)
    )
    {
        if (
            value >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()
            )
        )
        {
            throw std::overflow_error(error_message);
        }
    }

    return static_cast<std::size_t>(value);
}

bool is_valid_encoded_hash(
    const std::string& hash
)
{
    if (hash.size() != encoded_hash_size)
    {
        return false;
    }

    for (const char character : hash)
    {
        const bool is_decimal_digit =
            character >= '0' &&
            character <= '9';

        const bool is_lowercase_hexadecimal =
            character >= 'a' &&
            character <= 'f';

        if (
            !is_decimal_digit &&
            !is_lowercase_hexadecimal
        )
        {
            return false;
        }
    }

    return true;
}

}

std::vector<std::uint8_t> FileManifestCodec::encode(
    const FileManifest& manifest
)
{
    if (
        manifest.format_version() !=
        FileManifest::current_format_version
    )
    {
        throw std::runtime_error(
            "Unsupported file manifest format version."
        );
    }

    if (manifest.original_filename().empty())
    {
        throw std::runtime_error(
            "Cannot encode a manifest with an empty filename."
        );
    }

    if (manifest.chunk_size() == 0)
    {
        throw std::runtime_error(
            "Cannot encode a manifest with a zero chunk size."
        );
    }

    if constexpr (
        sizeof(std::size_t) >
        sizeof(std::uint64_t)
    )
    {
        if (
            manifest.chunk_size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max()
            )
        )
        {
            throw std::overflow_error(
                "Manifest chunk size exceeds the binary format limit."
            );
        }

        if (
            manifest.original_filename().size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max()
            )
        )
        {
            throw std::overflow_error(
                "Manifest filename length exceeds the binary format limit."
            );
        }

        if (
            manifest.chunk_count() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max()
            )
        )
        {
            throw std::overflow_error(
                "Manifest chunk count exceeds the binary format limit."
            );
        }
    }

    for (const std::string& hash : manifest.chunk_hashes())
    {
        if (!is_valid_encoded_hash(hash))
        {
            throw std::runtime_error(
                "Cannot encode an invalid SHA-256 chunk hash."
            );
        }
    }

    constexpr std::size_t fixed_header_size =
        manifest_magic.size()
        + sizeof(std::uint32_t)
        + sizeof(std::uint64_t)
        + sizeof(std::uint64_t)
        + sizeof(std::uint64_t)
        + sizeof(std::uint64_t);

    const std::size_t encoded_hashes_size =
        checked_multiply(
            manifest.chunk_count(),
            encoded_hash_size
        );

    std::size_t encoded_size =
        checked_add(
            fixed_header_size,
            manifest.original_filename().size()
        );

    encoded_size =
        checked_add(
            encoded_size,
            encoded_hashes_size
        );

    std::vector<std::uint8_t> encoded;
    encoded.reserve(encoded_size);

    encoded.insert(
        encoded.end(),
        manifest_magic.begin(),
        manifest_magic.end()
    );

    append_uint32_big_endian(
        encoded,
        manifest.format_version()
    );

    append_uint64_big_endian(
        encoded,
        manifest.file_size()
    );

    append_uint64_big_endian(
        encoded,
        static_cast<std::uint64_t>(
            manifest.chunk_size()
        )
    );

    append_uint64_big_endian(
        encoded,
        static_cast<std::uint64_t>(
            manifest.original_filename().size()
        )
    );

    append_text(
        encoded,
        manifest.original_filename()
    );

    append_uint64_big_endian(
        encoded,
        static_cast<std::uint64_t>(
            manifest.chunk_count()
        )
    );

    for (const std::string& hash : manifest.chunk_hashes())
    {
        append_text(encoded, hash);
    }

    if (encoded.size() != encoded_size)
    {
        throw std::runtime_error(
            "Encoded manifest size does not match the expected size."
        );
    }

    return encoded;
}

FileManifest FileManifestCodec::decode(
    std::span<const std::uint8_t> encoded_manifest
)
{
    ByteReader reader{encoded_manifest};

    for (const std::uint8_t expected_byte : manifest_magic)
    {
        const std::uint8_t actual_byte =
            reader.read_byte();

        if (actual_byte != expected_byte)
        {
            throw std::runtime_error(
                "Encoded manifest has an invalid magic signature."
            );
        }
    }

    const std::uint32_t format_version =
        reader.read_uint32_big_endian();

    if (
        format_version !=
        FileManifest::current_format_version
    )
    {
        throw std::runtime_error(
            "Encoded manifest uses an unsupported format version."
        );
    }

    const std::uint64_t file_size =
        reader.read_uint64_big_endian();

    const std::uint64_t encoded_chunk_size =
        reader.read_uint64_big_endian();

    const std::uint64_t encoded_filename_length =
        reader.read_uint64_big_endian();

    const std::size_t filename_length =
        convert_uint64_to_size(
            encoded_filename_length,
            "Manifest filename length exceeds the platform limit."
        );

    std::string original_filename =
        reader.read_text(filename_length);

    const std::uint64_t encoded_chunk_count =
        reader.read_uint64_big_endian();

    const std::size_t chunk_count =
        convert_uint64_to_size(
            encoded_chunk_count,
            "Manifest chunk count exceeds the platform limit."
        );

    const std::size_t expected_hashes_size =
        checked_multiply(
            chunk_count,
            encoded_hash_size
        );

    if (reader.remaining() < expected_hashes_size)
    {
        throw std::runtime_error(
            "Encoded manifest chunk hashes are truncated."
        );
    }

    if (reader.remaining() > expected_hashes_size)
    {
        throw std::runtime_error(
            "Encoded manifest contains trailing or inconsistent data."
        );
    }

    std::vector<std::string> chunk_hashes;
    chunk_hashes.reserve(chunk_count);

    for (
        std::size_t index = 0;
        index < chunk_count;
        ++index
    )
    {
        std::string hash =
            reader.read_text(encoded_hash_size);

        if (!is_valid_encoded_hash(hash))
        {
            throw std::runtime_error(
                "Encoded manifest contains an invalid chunk hash."
            );
        }

        chunk_hashes.push_back(
            std::move(hash)
        );
    }

    if (!reader.at_end())
    {
        throw std::runtime_error(
            "Encoded manifest contains trailing bytes."
        );
    }

    const std::size_t chunk_size =
        convert_uint64_to_size(
            encoded_chunk_size,
            "Manifest chunk size exceeds the platform limit."
        );

    return FileManifest::restore(
        std::move(original_filename),
        file_size,
        chunk_size,
        std::move(chunk_hashes)
    );
}

}