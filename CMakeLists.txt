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
)

target_include_directories(nexusfs
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(nexusfs PRIVATE /W4 /permissive-)
else()
    target_compile_options(nexusfs PRIVATE -Wall -Wextra -Wpedantic)
endif()