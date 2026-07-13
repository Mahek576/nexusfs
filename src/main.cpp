#include "nexusfs/app/nexusfs_service.hpp"
#include "nexusfs/cli/command_line.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <variant>

namespace
{

const std::filesystem::path storage_root{
    "nexusfs_data"
};

constexpr std::size_t default_chunk_size = 1024;

int execute_store(
    const nexusfs::app::NexusFsService& service,
    const nexusfs::cli::StoreCommand& command
)
{
    const auto result =
        service.store_file(
            command.source_path
        );

    std::cout << "Command: store\n";

    std::cout << "Source file: "
              << result.source_path
              << '\n';

    std::cout << "Storage root: "
              << service.storage_root()
              << '\n';

    std::cout << "Configured chunk size: "
              << service.default_chunk_size()
              << " bytes\n";

    std::cout << "Original filename: "
              << result.original_filename
              << '\n';

    std::cout << "Original file size: "
              << result.file_size
              << " bytes\n";

    std::cout << "Total chunks: "
              << result.chunk_count
              << '\n';

    std::cout << "New chunks stored: "
              << result.chunks_stored
              << '\n';

    std::cout << "Existing chunks reused: "
              << result.chunks_reused
              << '\n';

    std::cout << "Total bytes processed: "
              << result.bytes_processed
              << '\n';

    std::cout << "Encoded manifest size: "
              << result.encoded_manifest_size
              << " bytes\n";

    std::cout << "Manifest storage result: "
              << (
                  result.manifest_stored
                      ? "stored"
                      : "reused"
              )
              << '\n';

    std::cout << "Manifest ID: "
              << result.manifest_id
              << '\n';

    std::cout
        << "File stored successfully.\n";

    return 0;
}

int execute_restore(
    const nexusfs::app::NexusFsService& service,
    const nexusfs::cli::RestoreCommand& command
)
{
    const auto result =
        service.restore_file(
            command.manifest_id,
            command.output_path
        );

    std::cout << "Command: restore\n";

    std::cout << "Manifest ID: "
              << result.manifest_id
              << '\n';

    std::cout << "Original filename: "
              << result.original_filename
              << '\n';

    std::cout << "Manifest file size: "
              << result.file_size
              << " bytes\n";

    std::cout << "Manifest chunk references: "
              << result.chunk_count
              << '\n';

    std::cout << "Output path: "
              << result.output_path
              << '\n';

    std::cout << "Reconstructed bytes written: "
              << result.bytes_written
              << '\n';

    std::cout << "Reconstructed chunks loaded: "
              << result.chunks_loaded
              << '\n';

    std::cout
        << "File restored successfully.\n";

    return 0;
}

int execute_inspect(
    const nexusfs::app::NexusFsService& service,
    const nexusfs::cli::InspectCommand& command
)
{
    const auto result =
        service.inspect_file(
            command.manifest_id
        );

    std::cout << "Command: inspect\n";

    std::cout << "Manifest ID: "
              << result.manifest_id
              << '\n';

    std::cout << "Storage root: "
              << service.storage_root()
              << '\n';

    std::cout << "Original filename: "
              << result.original_filename
              << '\n';

    std::cout << "Original file size: "
              << result.file_size
              << " bytes\n";

    std::cout << "Configured chunk size: "
              << result.configured_chunk_size
              << " bytes\n";

    std::cout << "Encoded manifest size: "
              << result.encoded_manifest_size
              << " bytes\n";

    std::cout << "Chunk references: "
              << result.chunks.size()
              << '\n';

    for (const auto& chunk : result.chunks)
    {
        std::cout << "Chunk reference "
                  << chunk.index
                  << ": "
                  << chunk.hash
                  << " | "
                  << (
                      chunk.present
                          ? "present"
                          : "missing"
                  )
                  << '\n';
    }

    std::cout << "Available chunks: "
              << result.available_chunks
              << '\n';

    std::cout << "Missing chunks: "
              << result.missing_chunks
              << '\n';

    std::cout << "Storage completeness: "
              << (
                  result.missing_chunks == 0
                      ? "complete"
                      : "incomplete"
              )
              << '\n';

    std::cout
        << "Manifest inspected successfully.\n";

    return 0;
}

int execute_verify(
    const nexusfs::app::NexusFsService& service,
    const nexusfs::cli::VerifyCommand& command
)
{
    const auto result =
        service.verify_file(
            command.manifest_id
        );

    std::cout << "Command: verify\n";

    std::cout << "Manifest ID: "
              << result.manifest_id
              << '\n';

    std::cout << "Storage root: "
              << service.storage_root()
              << '\n';

    std::cout << "Original filename: "
              << result.original_filename
              << '\n';

    std::cout << "Manifest file size: "
              << result.file_size
              << " bytes\n";

    std::cout << "Manifest chunk references: "
              << result.chunk_count
              << '\n';

    for (
        const auto& verified_chunk :
        result.verified_chunks
    )
    {
        std::cout << "Verified chunk "
                  << verified_chunk.index
                  << ": "
                  << verified_chunk.hash
                  << " | "
                  << verified_chunk.bytes_verified
                  << " bytes\n";
    }

    std::cout << "Verified chunks: "
              << result.verified_chunks.size()
              << '\n';

    std::cout << "Total bytes verified: "
              << result.total_bytes_verified
              << '\n';

    std::cout << "Storage integrity: healthy\n";

    std::cout
        << "File storage verified successfully.\n";

    return 0;
}

int execute_list(
    const nexusfs::app::NexusFsService& service,
    const nexusfs::cli::ListCommand&
)
{
    const auto result =
        service.list_files();

    std::cout << "Command: list\n";

    std::cout << "Storage root: "
              << service.storage_root()
              << '\n';

    std::cout << "Stored manifests: "
              << result.files.size()
              << '\n';

    if (result.files.empty())
    {
        std::cout
            << "No stored manifests found.\n";

        return 0;
    }

    for (
        std::size_t index = 0;
        index < result.files.size();
        ++index
    )
    {
        const auto& file =
            result.files[index];

        std::cout << "Manifest "
                  << index
                  << ":\n";

        std::cout << "  ID: "
                  << file.manifest_id
                  << '\n';

        std::cout << "  Filename: "
                  << file.original_filename
                  << '\n';

        std::cout << "  File size: "
                  << file.file_size
                  << " bytes\n";

        std::cout << "  Chunk size: "
                  << file.configured_chunk_size
                  << " bytes\n";

        std::cout << "  Chunk references: "
                  << file.chunk_count
                  << '\n';

        std::cout << "  Missing chunks: "
                  << file.missing_chunks
                  << '\n';

        std::cout << "  Storage status: "
                  << (
                      file.missing_chunks == 0
                          ? "complete"
                          : "incomplete"
                  )
                  << '\n';
    }

    std::cout << "Complete manifests: "
              << result.complete_manifests
              << '\n';

    std::cout << "Incomplete manifests: "
              << result.incomplete_manifests
              << '\n';

    std::cout
        << "Stored manifests listed successfully.\n";

    return 0;
}

}

int main(int argc, char* argv[])
{
    try
    {
        const nexusfs::app::NexusFsService service{
            storage_root,
            default_chunk_size
        };

        const nexusfs::cli::Command command =
            nexusfs::cli::CommandLineParser::parse(
                argc,
                argv
            );

        if (
            const auto* store_command =
                std::get_if<nexusfs::cli::StoreCommand>(
                    &command
                )
        )
        {
            return execute_store(
                service,
                *store_command
            );
        }

        if (
            const auto* restore_command =
                std::get_if<nexusfs::cli::RestoreCommand>(
                    &command
                )
        )
        {
            return execute_restore(
                service,
                *restore_command
            );
        }

        if (
            const auto* inspect_command =
                std::get_if<nexusfs::cli::InspectCommand>(
                    &command
                )
        )
        {
            return execute_inspect(
                service,
                *inspect_command
            );
        }

        if (
            const auto* verify_command =
                std::get_if<nexusfs::cli::VerifyCommand>(
                    &command
                )
        )
        {
            return execute_verify(
                service,
                *verify_command
            );
        }

        return execute_list(
            service,
            std::get<nexusfs::cli::ListCommand>(
                command
            )
        );
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "NexusFS error: "
            << error.what()
            << '\n';

        return 1;
    }
}