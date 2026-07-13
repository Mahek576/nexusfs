#include "nexusfs/storage/chunk_store.hpp"
#include "nexusfs/storage/chunker.hpp"
#include "nexusfs/storage/file_manifest.hpp"
#include "nexusfs/storage/file_manifest_codec.hpp"
#include "nexusfs/storage/file_reconstructor.hpp"
#include "nexusfs/storage/file_verifier.hpp"
#include "nexusfs/storage/manifest_store.hpp"
#include "nexusfs/storage/sha256_hasher.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        const auto timestamp =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();

        path_ =
            std::filesystem::temp_directory_path()
            / (
                "nexusfs-tests-"
                + std::to_string(timestamp)
            );

        std::error_code error;

        std::filesystem::create_directories(
            path_,
            error
        );

        if (error)
        {
            throw std::runtime_error(
                "Failed to create temporary test directory: "
                + error.message()
            );
        }
    }

    TemporaryDirectory(
        const TemporaryDirectory&
    ) = delete;

    TemporaryDirectory& operator=(
        const TemporaryDirectory&
    ) = delete;

    ~TemporaryDirectory()
    {
        std::error_code error;

        std::filesystem::remove_all(
            path_,
            error
        );
    }

    [[nodiscard]] const std::filesystem::path&
    path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void require_true(
    bool condition,
    const std::string& test_name
)
{
    if (!condition)
    {
        throw std::runtime_error(
            test_name + " failed."
        );
    }
}

void require_equal(
    const std::string& actual,
    const std::string& expected,
    const std::string& test_name
)
{
    if (actual != expected)
    {
        throw std::runtime_error(
            test_name
            + " failed.\nExpected: "
            + expected
            + "\nActual:   "
            + actual
        );
    }
}

template <typename Actual, typename Expected>
void require_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& test_name
)
{
    if (actual != expected)
    {
        throw std::runtime_error(
            test_name + " failed."
        );
    }
}

template <typename Function>
void require_throws(
    Function&& function,
    const std::string& test_name
)
{
    bool exception_thrown = false;

    try
    {
        std::forward<Function>(function)();
    }
    catch (const std::exception&)
    {
        exception_thrown = true;
    }

    if (!exception_thrown)
    {
        throw std::runtime_error(
            test_name
            + " failed because no exception was thrown."
        );
    }
}

std::vector<std::uint8_t> to_bytes(
    const std::string& text
)
{
    return std::vector<std::uint8_t>{
        text.begin(),
        text.end()
    };
}

std::vector<std::uint8_t> create_binary_test_data(
    std::size_t size
)
{
    std::vector<std::uint8_t> data;
    data.reserve(size);

    for (std::size_t index = 0; index < size; ++index)
    {
        const auto value =
            static_cast<std::uint8_t>(
                ((index * 37U) + 11U) % 256U
            );

        data.push_back(value);
    }

    return data;
}

void write_binary_file(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& data
)
{
    const std::filesystem::path parent =
        path.parent_path();

    if (!parent.empty())
    {
        std::error_code directory_error;

        std::filesystem::create_directories(
            parent,
            directory_error
        );

        if (directory_error)
        {
            throw std::runtime_error(
                "Failed to create test file directory: "
                + directory_error.message()
            );
        }
    }

    if (
        data.size() >
        static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max()
        )
    )
    {
        throw std::runtime_error(
            "Test data is too large for stream writing."
        );
    }

    std::ofstream output{
        path,
        std::ios::binary | std::ios::trunc
    };

    if (!output.is_open())
    {
        throw std::runtime_error(
            "Failed to create test file: "
            + path.string()
        );
    }

    if (!data.empty())
    {
        output.write(
            reinterpret_cast<const char*>(
                data.data()
            ),
            static_cast<std::streamsize>(
                data.size()
            )
        );
    }

    output.flush();

    if (!output)
    {
        throw std::runtime_error(
            "Failed while writing test file: "
            + path.string()
        );
    }
}

std::vector<std::uint8_t> read_binary_file(
    const std::filesystem::path& path
)
{
    std::error_code error;

    const std::uintmax_t file_size =
        std::filesystem::file_size(
            path,
            error
        );

    if (error)
    {
        throw std::runtime_error(
            "Failed to determine test file size: "
            + error.message()
        );
    }

    if (
        file_size >
        static_cast<std::uintmax_t>(
            std::numeric_limits<std::size_t>::max()
        )
    )
    {
        throw std::runtime_error(
            "Test file is too large to load."
        );
    }

    if (
        file_size >
        static_cast<std::uintmax_t>(
            std::numeric_limits<std::streamsize>::max()
        )
    )
    {
        throw std::runtime_error(
            "Test file is too large for stream reading."
        );
    }

    std::ifstream input{
        path,
        std::ios::binary
    };

    if (!input.is_open())
    {
        throw std::runtime_error(
            "Failed to open test file: "
            + path.string()
        );
    }

    std::vector<std::uint8_t> data(
        static_cast<std::size_t>(file_size)
    );

    if (!data.empty())
    {
        input.read(
            reinterpret_cast<char*>(
                data.data()
            ),
            static_cast<std::streamsize>(
                data.size()
            )
        );

        if (!input)
        {
            throw std::runtime_error(
                "Failed while reading test file: "
                + path.string()
            );
        }
    }

    return data;
}

std::vector<std::string> create_test_hashes()
{
    return {
        std::string(64, 'a'),
        std::string(64, 'b'),
        std::string(64, 'c')
    };
}

void test_sha256_empty_input()
{
    const std::vector<std::uint8_t> input;

    const std::string actual =
        nexusfs::storage::Sha256Hasher::hash(
            std::span<const std::uint8_t>{
                input
            }
        );

    const std::string expected =
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855";

    require_equal(
        actual,
        expected,
        "SHA-256 empty-input test"
    );
}

void test_sha256_abc()
{
    const auto input =
        to_bytes("abc");

    const std::string actual =
        nexusfs::storage::Sha256Hasher::hash(
            std::span<const std::uint8_t>{
                input
            }
        );

    const std::string expected =
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad";

    require_equal(
        actual,
        expected,
        "SHA-256 abc test"
    );
}

void test_manifest_round_trip()
{
    constexpr std::uint64_t file_size = 2500;
    constexpr std::size_t chunk_size = 1024;

    const auto expected_hashes =
        create_test_hashes();

    const auto original_manifest =
        nexusfs::storage::FileManifest::restore(
            "example.bin",
            file_size,
            chunk_size,
            expected_hashes
        );

    const auto encoded_manifest =
        nexusfs::storage::FileManifestCodec::encode(
            original_manifest
        );

    const auto decoded_manifest =
        nexusfs::storage::FileManifestCodec::decode(
            encoded_manifest
        );

    require_equal(
        decoded_manifest.format_version(),
        nexusfs::storage::FileManifest::
            current_format_version,
        "Manifest format-version round-trip test"
    );

    require_equal(
        decoded_manifest.original_filename(),
        std::string{"example.bin"},
        "Manifest filename round-trip test"
    );

    require_equal(
        decoded_manifest.file_size(),
        file_size,
        "Manifest file-size round-trip test"
    );

    require_equal(
        decoded_manifest.chunk_size(),
        chunk_size,
        "Manifest chunk-size round-trip test"
    );

    require_equal(
        decoded_manifest.chunk_count(),
        expected_hashes.size(),
        "Manifest chunk-count round-trip test"
    );

    require_equal(
        decoded_manifest.chunk_hashes(),
        expected_hashes,
        "Manifest chunk-hash round-trip test"
    );

    const auto reencoded_manifest =
        nexusfs::storage::FileManifestCodec::encode(
            decoded_manifest
        );

    require_equal(
        reencoded_manifest,
        encoded_manifest,
        "Manifest canonical re-encoding test"
    );
}

void test_manifest_encoding_is_deterministic()
{
    constexpr std::uint64_t file_size = 2500;
    constexpr std::size_t chunk_size = 1024;

    const auto manifest =
        nexusfs::storage::FileManifest::restore(
            "example.bin",
            file_size,
            chunk_size,
            create_test_hashes()
        );

    const auto first_encoding =
        nexusfs::storage::FileManifestCodec::encode(
            manifest
        );

    const auto second_encoding =
        nexusfs::storage::FileManifestCodec::encode(
            manifest
        );

    require_equal(
        first_encoding,
        second_encoding,
        "Deterministic manifest encoding test"
    );

    const std::string first_id =
        nexusfs::storage::Sha256Hasher::hash(
            first_encoding
        );

    const std::string second_id =
        nexusfs::storage::Sha256Hasher::hash(
            second_encoding
        );

    require_equal(
        first_id,
        second_id,
        "Deterministic manifest ID test"
    );
}

void test_manifest_rejects_truncated_data()
{
    const auto manifest =
        nexusfs::storage::FileManifest::restore(
            "example.bin",
            2500,
            1024,
            create_test_hashes()
        );

    auto encoded_manifest =
        nexusfs::storage::FileManifestCodec::encode(
            manifest
        );

    require_true(
        !encoded_manifest.empty(),
        "Manifest test encoding must not be empty"
    );

    encoded_manifest.pop_back();

    require_throws(
        [&encoded_manifest]()
        {
            (void)nexusfs::storage::
                FileManifestCodec::decode(
                    encoded_manifest
                );
        },
        "Truncated manifest rejection test"
    );
}

void test_manifest_rejects_trailing_data()
{
    const auto manifest =
        nexusfs::storage::FileManifest::restore(
            "example.bin",
            2500,
            1024,
            create_test_hashes()
        );

    auto encoded_manifest =
        nexusfs::storage::FileManifestCodec::encode(
            manifest
        );

    encoded_manifest.push_back(
        static_cast<std::uint8_t>(0x00)
    );

    require_throws(
        [&encoded_manifest]()
        {
            (void)nexusfs::storage::
                FileManifestCodec::decode(
                    encoded_manifest
                );
        },
        "Trailing manifest data rejection test"
    );
}

void run_single_chunk_file_test(
    std::size_t file_size,
    const std::string& filename,
    const std::string& test_name
)
{
    constexpr std::size_t chunk_size = 1024;

    TemporaryDirectory temporary_directory;

    const std::filesystem::path source_path =
        temporary_directory.path()
        / filename;

    const std::filesystem::path storage_root =
        temporary_directory.path()
        / "storage";

    const std::filesystem::path restored_path =
        temporary_directory.path()
        / "restored"
        / filename;

    const auto original_data =
        create_binary_test_data(file_size);

    write_binary_file(
        source_path,
        original_data
    );

    const nexusfs::storage::Chunker chunker{
        chunk_size
    };

    const auto chunks =
        chunker.split_file(source_path);

    require_equal(
        chunks.size(),
        static_cast<std::size_t>(1),
        test_name + " chunk-count test"
    );

    require_equal(
        chunks.front().index,
        static_cast<std::size_t>(0),
        test_name + " chunk-index test"
    );

    require_equal(
        chunks.front().data.size(),
        file_size,
        test_name + " chunk-size test"
    );

    require_equal(
        chunks.front().data,
        original_data,
        test_name + " chunk-data test"
    );

    nexusfs::storage::ChunkStore chunk_store{
        storage_root
    };

    const auto chunk_store_result =
        chunk_store.store(
            chunks.front()
        );

    require_equal(
        chunk_store_result,
        nexusfs::storage::StoreResult::stored,
        test_name + " chunk-storage test"
    );

    const auto stored_chunk =
        chunk_store.load(
            chunks.front().hash
        );

    require_equal(
        stored_chunk,
        original_data,
        test_name + " chunk read-back test"
    );

    const auto manifest =
        nexusfs::storage::FileManifest::create(
            source_path,
            chunker.chunk_size(),
            chunks
        );

    require_equal(
        manifest.file_size(),
        static_cast<std::uint64_t>(file_size),
        test_name + " manifest file-size test"
    );

    require_equal(
        manifest.chunk_size(),
        chunk_size,
        test_name + " manifest chunk-size test"
    );

    require_equal(
        manifest.chunk_count(),
        static_cast<std::size_t>(1),
        test_name + " manifest chunk-count test"
    );

    require_equal(
        manifest.chunk_hashes().front(),
        chunks.front().hash,
        test_name + " manifest chunk-hash test"
    );

    const auto encoded_manifest =
        nexusfs::storage::FileManifestCodec::encode(
            manifest
        );

    const std::string manifest_id =
        nexusfs::storage::Sha256Hasher::hash(
            encoded_manifest
        );

    nexusfs::storage::ManifestStore manifest_store{
        storage_root
    };

    const auto manifest_store_result =
        manifest_store.store(
            manifest_id,
            encoded_manifest
        );

    require_equal(
        manifest_store_result,
        nexusfs::storage::ManifestStoreResult::stored,
        test_name + " manifest-storage test"
    );

    const auto loaded_manifest_bytes =
        manifest_store.load(
            manifest_id
        );

    const auto decoded_manifest =
        nexusfs::storage::FileManifestCodec::decode(
            loaded_manifest_bytes
        );

    const auto verification_result =
        nexusfs::storage::FileVerifier::verify(
            decoded_manifest,
            chunk_store
        );

    require_equal(
        verification_result.verified_chunks.size(),
        static_cast<std::size_t>(1),
        test_name + " verified-chunk count test"
    );

    require_equal(
        verification_result.total_bytes_verified,
        static_cast<std::uint64_t>(file_size),
        test_name + " verified-byte count test"
    );

    require_equal(
        verification_result.verified_chunks.front()
            .bytes_verified,
        static_cast<std::uint64_t>(file_size),
        test_name + " verified chunk-size test"
    );

    const auto reconstruction_result =
        nexusfs::storage::FileReconstructor::reconstruct(
            decoded_manifest,
            chunk_store,
            restored_path
        );

    require_equal(
        reconstruction_result.chunks_loaded,
        static_cast<std::size_t>(1),
        test_name + " reconstructed-chunk count test"
    );

    require_equal(
        reconstruction_result.bytes_written,
        static_cast<std::uint64_t>(file_size),
        test_name + " reconstructed-byte count test"
    );

    const auto restored_data =
        read_binary_file(restored_path);

    require_equal(
        restored_data,
        original_data,
        test_name + " reconstructed-data test"
    );
}

void test_sub_chunk_file_storage_pipeline()
{
    run_single_chunk_file_test(
        137,
        "small.bin",
        "Sub-chunk file"
    );
}

void test_exact_chunk_file_storage_pipeline()
{
    run_single_chunk_file_test(
        1024,
        "exact-chunk.bin",
        "Exact-chunk file"
    );
}

void test_empty_file_storage_pipeline()
{
    constexpr std::size_t chunk_size = 1024;

    TemporaryDirectory temporary_directory;

    const std::filesystem::path source_path =
        temporary_directory.path()
        / "empty.bin";

    const std::filesystem::path storage_root =
        temporary_directory.path()
        / "storage";

    const std::filesystem::path restored_path =
        temporary_directory.path()
        / "restored"
        / "empty.bin";

    write_binary_file(
        source_path,
        {}
    );

    const nexusfs::storage::Chunker chunker{
        chunk_size
    };

    const auto chunks =
        chunker.split_file(source_path);

    require_true(
        chunks.empty(),
        "Empty-file chunk list test"
    );

    const auto manifest =
        nexusfs::storage::FileManifest::create(
            source_path,
            chunker.chunk_size(),
            chunks
        );

    require_equal(
        manifest.original_filename(),
        std::string{"empty.bin"},
        "Empty-file manifest filename test"
    );

    require_equal(
        manifest.file_size(),
        static_cast<std::uint64_t>(0),
        "Empty-file manifest size test"
    );

    require_equal(
        manifest.chunk_size(),
        chunk_size,
        "Empty-file manifest chunk-size test"
    );

    require_equal(
        manifest.chunk_count(),
        static_cast<std::size_t>(0),
        "Empty-file manifest chunk-count test"
    );

    const auto encoded_manifest =
        nexusfs::storage::FileManifestCodec::encode(
            manifest
        );

    require_true(
        !encoded_manifest.empty(),
        "Empty-file encoded manifest test"
    );

    const std::string manifest_id =
        nexusfs::storage::Sha256Hasher::hash(
            encoded_manifest
        );

    nexusfs::storage::ManifestStore manifest_store{
        storage_root
    };

    const auto store_result =
        manifest_store.store(
            manifest_id,
            encoded_manifest
        );

    require_equal(
        store_result,
        nexusfs::storage::ManifestStoreResult::stored,
        "Empty-file manifest storage test"
    );

    const auto loaded_manifest_bytes =
        manifest_store.load(
            manifest_id
        );

    require_equal(
        loaded_manifest_bytes,
        encoded_manifest,
        "Empty-file manifest read-back test"
    );

    const auto decoded_manifest =
        nexusfs::storage::FileManifestCodec::decode(
            loaded_manifest_bytes
        );

    nexusfs::storage::ChunkStore chunk_store{
        storage_root
    };

    const auto verification_result =
        nexusfs::storage::FileVerifier::verify(
            decoded_manifest,
            chunk_store
        );

    require_equal(
        verification_result.total_bytes_verified,
        static_cast<std::uint64_t>(0),
        "Empty-file verified-byte count test"
    );

    require_true(
        verification_result.verified_chunks.empty(),
        "Empty-file verified-chunk list test"
    );

    const auto reconstruction_result =
        nexusfs::storage::FileReconstructor::reconstruct(
            decoded_manifest,
            chunk_store,
            restored_path
        );

    require_equal(
        reconstruction_result.bytes_written,
        static_cast<std::uint64_t>(0),
        "Empty-file reconstructed-byte count test"
    );

    require_equal(
        reconstruction_result.chunks_loaded,
        static_cast<std::size_t>(0),
        "Empty-file reconstructed-chunk count test"
    );

    require_true(
        std::filesystem::is_regular_file(
            restored_path
        ),
        "Empty-file reconstructed output existence test"
    );

    const auto restored_data =
        read_binary_file(
            restored_path
        );

    require_true(
        restored_data.empty(),
        "Empty-file reconstructed-data test"
    );

    const std::string original_hash =
        nexusfs::storage::Sha256Hasher::hash(
            read_binary_file(source_path)
        );

    const std::string restored_hash =
        nexusfs::storage::Sha256Hasher::hash(
            restored_data
        );

    require_equal(
        restored_hash,
        original_hash,
        "Empty-file reconstructed hash test"
    );
}

void test_end_to_end_storage_pipeline()
{
    constexpr std::size_t chunk_size = 1024;
    constexpr std::size_t test_file_size = 2500;

    TemporaryDirectory temporary_directory;

    const std::filesystem::path source_path =
        temporary_directory.path()
        / "source.bin";

    const std::filesystem::path storage_root =
        temporary_directory.path()
        / "storage";

    const std::filesystem::path restored_path =
        temporary_directory.path()
        / "restored"
        / "source.bin";

    const auto original_data =
        create_binary_test_data(
            test_file_size
        );

    write_binary_file(
        source_path,
        original_data
    );

    const nexusfs::storage::Chunker chunker{
        chunk_size
    };

    const auto chunks =
        chunker.split_file(source_path);

    require_equal(
        chunks.size(),
        static_cast<std::size_t>(3),
        "End-to-end chunk-count test"
    );

    require_equal(
        chunks[0].data.size(),
        static_cast<std::size_t>(1024),
        "End-to-end first-chunk size test"
    );

    require_equal(
        chunks[1].data.size(),
        static_cast<std::size_t>(1024),
        "End-to-end second-chunk size test"
    );

    require_equal(
        chunks[2].data.size(),
        static_cast<std::size_t>(452),
        "End-to-end final-chunk size test"
    );

    nexusfs::storage::ChunkStore chunk_store{
        storage_root
    };

    std::unordered_set<std::string> encountered_hashes;

    for (const auto& chunk : chunks)
    {
        const bool is_first_hash_occurrence =
            encountered_hashes.insert(
                chunk.hash
            ).second;

        const auto expected_first_result =
            is_first_hash_occurrence
                ? nexusfs::storage::StoreResult::stored
                : nexusfs::storage::StoreResult::already_exists;

        const auto first_result =
            chunk_store.store(chunk);

        require_equal(
            first_result,
            expected_first_result,
            "Initial chunk-storage result test"
        );

        const auto loaded_chunk =
            chunk_store.load(chunk.hash);

        require_equal(
            loaded_chunk,
            chunk.data,
            "Stored chunk read-back test"
        );

        const auto second_result =
            chunk_store.store(chunk);

        require_equal(
            second_result,
            nexusfs::storage::StoreResult::already_exists,
            "Repeated chunk-storage deduplication test"
        );
    }

    require_true(
        encountered_hashes.size() < chunks.size(),
        "End-to-end test must include intra-file deduplication"
    );

    const auto manifest =
        nexusfs::storage::FileManifest::create(
            source_path,
            chunker.chunk_size(),
            chunks
        );

    const auto encoded_manifest =
        nexusfs::storage::FileManifestCodec::encode(
            manifest
        );

    const std::string manifest_id =
        nexusfs::storage::Sha256Hasher::hash(
            encoded_manifest
        );

    nexusfs::storage::ManifestStore manifest_store{
        storage_root
    };

    const auto first_manifest_result =
        manifest_store.store(
            manifest_id,
            encoded_manifest
        );

    require_equal(
        first_manifest_result,
        nexusfs::storage::ManifestStoreResult::stored,
        "First manifest-storage result test"
    );

    const auto second_manifest_result =
        manifest_store.store(
            manifest_id,
            encoded_manifest
        );

    require_equal(
        second_manifest_result,
        nexusfs::storage::ManifestStoreResult::already_exists,
        "Manifest deduplication result test"
    );

    const auto loaded_manifest_bytes =
        manifest_store.load(manifest_id);

    require_equal(
        loaded_manifest_bytes,
        encoded_manifest,
        "Manifest read-back test"
    );

    const auto decoded_manifest =
        nexusfs::storage::FileManifestCodec::decode(
            loaded_manifest_bytes
        );

    const auto verification_result =
        nexusfs::storage::FileVerifier::verify(
            decoded_manifest,
            chunk_store
        );

    require_equal(
        verification_result.total_bytes_verified,
        static_cast<std::uint64_t>(
            test_file_size
        ),
        "End-to-end verified-byte count test"
    );

    require_equal(
        verification_result.verified_chunks.size(),
        chunks.size(),
        "End-to-end verified-chunk count test"
    );

    const auto reconstruction_result =
        nexusfs::storage::FileReconstructor::reconstruct(
            decoded_manifest,
            chunk_store,
            restored_path
        );

    require_equal(
        reconstruction_result.bytes_written,
        static_cast<std::uint64_t>(
            test_file_size
        ),
        "End-to-end reconstructed-byte count test"
    );

    require_equal(
        reconstruction_result.chunks_loaded,
        chunks.size(),
        "End-to-end reconstructed-chunk count test"
    );

    const auto restored_data =
        read_binary_file(restored_path);

    require_equal(
        restored_data,
        original_data,
        "End-to-end reconstructed-data test"
    );

    const std::string& first_chunk_hash =
        decoded_manifest.chunk_hashes().front();

    const std::filesystem::path first_chunk_path =
        storage_root
        / "chunks"
        / first_chunk_hash.substr(0, 2)
        / first_chunk_hash.substr(2);

    auto corrupted_chunk =
        read_binary_file(first_chunk_path);

    require_true(
        !corrupted_chunk.empty(),
        "Corruption test chunk must not be empty"
    );

    corrupted_chunk[0] ^=
        static_cast<std::uint8_t>(0x01);

    write_binary_file(
        first_chunk_path,
        corrupted_chunk
    );

    require_throws(
        [&decoded_manifest, &chunk_store]()
        {
            (void)nexusfs::storage::
                FileVerifier::verify(
                    decoded_manifest,
                    chunk_store
                );
        },
        "Corrupted chunk verification test"
    );
}

}

int main()
{
    try
    {
        test_sha256_empty_input();

        std::cout
            << "[PASS] SHA-256 empty input\n";

        test_sha256_abc();

        std::cout
            << "[PASS] SHA-256 abc\n";

        test_manifest_round_trip();

        std::cout
            << "[PASS] Manifest round trip\n";

        test_manifest_encoding_is_deterministic();

        std::cout
            << "[PASS] Deterministic manifest encoding\n";

        test_manifest_rejects_truncated_data();

        std::cout
            << "[PASS] Truncated manifest rejection\n";

        test_manifest_rejects_trailing_data();

        std::cout
            << "[PASS] Trailing manifest data rejection\n";

        test_empty_file_storage_pipeline();

        std::cout
            << "[PASS] Empty-file storage pipeline\n";

        test_sub_chunk_file_storage_pipeline();

        std::cout
            << "[PASS] Sub-chunk file storage pipeline\n";

        test_exact_chunk_file_storage_pipeline();

        std::cout
            << "[PASS] Exact-chunk file storage pipeline\n";

        test_end_to_end_storage_pipeline();

        std::cout
            << "[PASS] End-to-end storage pipeline\n";

        std::cout
            << "All NexusFS tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "[FAIL] "
            << error.what()
            << '\n';

        return 1;
    }
}