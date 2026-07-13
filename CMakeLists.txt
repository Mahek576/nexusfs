cmake_minimum_required(VERSION 3.20)

project(
    NexusFS
    VERSION 0.1.0
    DESCRIPTION "A distributed storage engine built from first principles"
    LANGUAGES CXX
)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)

add_library(nexusfs_core STATIC
    src/cli/command_line.cpp
    src/storage/chunker.cpp
    src/storage/sha256_hasher.cpp
    src/storage/chunk_store.cpp
    src/storage/file_manifest.cpp
    src/storage/file_manifest_codec.cpp
    src/storage/manifest_store.cpp
    src/storage/file_reconstructor.cpp
    src/storage/file_verifier.cpp
)

target_include_directories(nexusfs_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(nexusfs_core
    PUBLIC
        OpenSSL::Crypto
)

add_executable(nexusfs
    src/main.cpp
)

target_link_libraries(nexusfs
    PRIVATE
        nexusfs_core
)

if(BUILD_TESTING)
    add_executable(nexusfs_tests
        tests/test_main.cpp
    )

    target_link_libraries(nexusfs_tests
        PRIVATE
            nexusfs_core
    )

    add_test(
        NAME nexusfs_tests
        COMMAND nexusfs_tests
    )

    add_executable(nexusfs_cli_tests
        tests/test_cli.cpp
    )

    target_link_libraries(nexusfs_cli_tests
        PRIVATE
            nexusfs_core
    )

    add_test(
        NAME nexusfs_cli_tests
        COMMAND nexusfs_cli_tests
    )
endif()

if(MSVC)
    target_compile_options(
        nexusfs_core
        PRIVATE
            /W4
            /permissive-
    )

    target_compile_options(
        nexusfs
        PRIVATE
            /W4
            /permissive-
    )

    if(BUILD_TESTING)
        target_compile_options(
            nexusfs_tests
            PRIVATE
                /W4
                /permissive-
        )

        target_compile_options(
            nexusfs_cli_tests
            PRIVATE
                /W4
                /permissive-
        )
    endif()
else()
    target_compile_options(
        nexusfs_core
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_options(
        nexusfs
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    if(BUILD_TESTING)
        target_compile_options(
            nexusfs_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            nexusfs_cli_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()
endif()