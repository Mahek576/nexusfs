cmake_minimum_required(VERSION 3.20)

project(
    NexusFS
    VERSION 0.1.0
    DESCRIPTION "A distributed storage engine built from first principles"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(nexusfs
    src/main.cpp
    src/storage/chunker.cpp
    src/storage/sha256_hasher.cpp
    src/storage/chunk_store.cpp
    src/storage/file_manifest.cpp
    src/storage/file_manifest_codec.cpp
    src/storage/manifest_store.cpp
    src/storage/file_reconstructor.cpp
)

target_include_directories(nexusfs
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

find_package(OpenSSL REQUIRED)

target_link_libraries(nexusfs
    PRIVATE
        OpenSSL::Crypto
)

if(MSVC)
    target_compile_options(nexusfs PRIVATE /W4 /permissive-)
else()
    target_compile_options(nexusfs PRIVATE -Wall -Wextra -Wpedantic)
endif()