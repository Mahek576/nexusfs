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
find_package(Boost CONFIG REQUIRED COMPONENTS beast)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(
    nexusfs_core
    STATIC
        src/app/nexusfs_service.cpp
        src/cli/command_line.cpp
        src/cluster/cluster_node_foundation.cpp
        src/daemon/daemon_configuration.cpp
        src/http/http_router.cpp
        src/http/http_router_cluster.cpp
        src/http/http_router_observability.cpp
        src/http/http_router_logging.cpp
        src/http/http_server.cpp
        src/http/http_session.cpp
        src/observability/json_logger.cpp
        src/observability/metrics_registry.cpp
        src/storage/durable_file.cpp
        src/storage/storage_recovery.cpp
        src/storage/chunker.cpp
        src/storage/sha256_hasher.cpp
        src/storage/chunk_store.cpp
        src/storage/file_manifest.cpp
        src/storage/file_manifest_codec.cpp
        src/storage/manifest_store.cpp
        src/storage/file_reconstructor.cpp
        src/storage/file_verifier.cpp
)

target_include_directories(
    nexusfs_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(
    nexusfs_core
    PUBLIC
        OpenSSL::Crypto
        Boost::beast
    PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads
)

if(WIN32)
    target_compile_definitions(
        nexusfs_core
        PUBLIC
            _WIN32_WINNT=0x0A00
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
endif()

add_executable(
    nexusfs
    src/main.cpp
)

target_link_libraries(
    nexusfs
    PRIVATE
        nexusfs_core
)

add_executable(
    nexusfsd
    src/daemon_main.cpp
)

target_link_libraries(
    nexusfsd
    PRIVATE
        nexusfs_core
)

if(BUILD_TESTING)
    add_executable(
        nexusfs_http_cluster_tests
        tests/test_http_cluster.cpp
    )

    target_link_libraries(
        nexusfs_http_cluster_tests
        PRIVATE
            nexusfs_core
            nlohmann_json::nlohmann_json
            Threads::Threads
    )

    add_test(
        NAME nexusfs_http_cluster_tests
        COMMAND nexusfs_http_cluster_tests
    )

    set_tests_properties(
        nexusfs_http_cluster_tests
        PROPERTIES
            TIMEOUT 30
    )

    add_executable(
        nexusfs_cluster_node_foundation_tests
        tests/test_cluster_node_foundation.cpp
    )

    target_link_libraries(
        nexusfs_cluster_node_foundation_tests
        PRIVATE
            nexusfs_core
            nlohmann_json::nlohmann_json
            Threads::Threads
    )

    add_test(
        NAME nexusfs_cluster_node_foundation_tests
        COMMAND nexusfs_cluster_node_foundation_tests
    )

    set_tests_properties(
        nexusfs_cluster_node_foundation_tests
        PROPERTIES
            TIMEOUT 30
    )

    add_executable(
        nexusfs_storage_recovery_tests
        tests/test_storage_recovery.cpp
    )

    target_link_libraries(
        nexusfs_storage_recovery_tests
        PRIVATE
            nexusfs_core
            nlohmann_json::nlohmann_json
            Threads::Threads
    )

    add_test(
        NAME nexusfs_storage_recovery_tests
        COMMAND nexusfs_storage_recovery_tests
    )

    set_tests_properties(
        nexusfs_storage_recovery_tests
        PROPERTIES
            TIMEOUT 30
    )

    add_executable(
        nexusfs_durable_file_tests
        tests/test_durable_file.cpp
    )

    target_link_libraries(
        nexusfs_durable_file_tests
        PRIVATE
            nexusfs_core
            Threads::Threads
    )

    add_test(
        NAME nexusfs_durable_file_tests
        COMMAND nexusfs_durable_file_tests
    )

    set_tests_properties(
        nexusfs_durable_file_tests
        PROPERTIES
            TIMEOUT 30
    )
    add_executable(
        nexusfs_tests
        tests/test_main.cpp
    )

    target_link_libraries(
        nexusfs_tests
        PRIVATE
            nexusfs_core
    )

    add_test(
        NAME nexusfs_tests
        COMMAND nexusfs_tests
    )

    add_executable(
        nexusfs_cli_tests
        tests/test_cli.cpp
    )

    target_link_libraries(
        nexusfs_cli_tests
        PRIVATE
            nexusfs_core
    )

    add_test(
        NAME nexusfs_cli_tests
        COMMAND nexusfs_cli_tests
    )

    add_executable(
        nexusfs_service_tests
        tests/test_service.cpp
    )

    target_link_libraries(
        nexusfs_service_tests
        PRIVATE
            nexusfs_core
    )

    add_test(
        NAME nexusfs_service_tests
        COMMAND nexusfs_service_tests
    )

    add_executable(
        nexusfs_http_tests
        tests/test_http.cpp
    )

    target_link_libraries(
        nexusfs_http_tests
        PRIVATE
            nexusfs_core
            nlohmann_json::nlohmann_json
    )

    add_test(
        NAME nexusfs_http_tests
        COMMAND nexusfs_http_tests
    )

    add_executable(
        nexusfs_http_upload_tests
        tests/test_http_upload.cpp
    )

    target_link_libraries(
        nexusfs_http_upload_tests
        PRIVATE
            nexusfs_core
            nlohmann_json::nlohmann_json
    )

    add_test(
        NAME nexusfs_http_upload_tests
        COMMAND nexusfs_http_upload_tests
    )

    add_executable(
        nexusfs_daemon_configuration_tests
        tests/test_daemon_configuration.cpp
    )

    target_link_libraries(
        nexusfs_daemon_configuration_tests
        PRIVATE
            nexusfs_core
    )

    add_test(
        NAME nexusfs_daemon_configuration_tests
        COMMAND nexusfs_daemon_configuration_tests
    )

    add_executable(
        nexusfs_http_server_lifecycle_tests
        tests/test_http_server_lifecycle.cpp
    )

    target_link_libraries(
        nexusfs_http_server_lifecycle_tests
        PRIVATE
            nexusfs_core
            Threads::Threads
    )

    add_test(
        NAME nexusfs_http_server_lifecycle_tests
        COMMAND nexusfs_http_server_lifecycle_tests
    )

    set_tests_properties(
        nexusfs_http_server_lifecycle_tests
        PROPERTIES
            TIMEOUT 30
    )

    add_executable(
        nexusfs_service_concurrency_tests
        tests/test_service_concurrency.cpp
    )

    target_link_libraries(
        nexusfs_service_concurrency_tests
        PRIVATE
            nexusfs_core
            Threads::Threads
    )

    add_test(
        NAME nexusfs_service_concurrency_tests
        COMMAND nexusfs_service_concurrency_tests
    )

    set_tests_properties(
        nexusfs_service_concurrency_tests
        PROPERTIES
            TIMEOUT 60
    )

    add_executable(
        nexusfs_json_logger_tests
        tests/test_json_logger.cpp
    )

    target_link_libraries(
        nexusfs_json_logger_tests
        PRIVATE
            nexusfs_core
            nlohmann_json::nlohmann_json
            Threads::Threads
    )

    add_test(
        NAME nexusfs_json_logger_tests
        COMMAND nexusfs_json_logger_tests
    )

    set_tests_properties(
        nexusfs_json_logger_tests
        PROPERTIES
            TIMEOUT 30
    )
    add_executable(
        nexusfs_metrics_registry_tests
        tests/test_metrics_registry.cpp
    )

    target_link_libraries(
        nexusfs_metrics_registry_tests
        PRIVATE
            nexusfs_core
            Threads::Threads
    )

    add_test(
        NAME nexusfs_metrics_registry_tests
        COMMAND nexusfs_metrics_registry_tests
    )

    set_tests_properties(
        nexusfs_metrics_registry_tests
        PROPERTIES
            TIMEOUT 30
    )

    add_executable(
        nexusfs_http_logging_tests
        tests/test_http_logging.cpp
    )

    target_link_libraries(
        nexusfs_http_logging_tests
        PRIVATE
            nexusfs_core
            nlohmann_json::nlohmann_json
            Threads::Threads
    )

    add_test(
        NAME nexusfs_http_logging_tests
        COMMAND nexusfs_http_logging_tests
    )

    set_tests_properties(
        nexusfs_http_logging_tests
        PROPERTIES
            TIMEOUT 30
    )
    add_executable(
        nexusfs_http_metrics_tests
        tests/test_http_metrics.cpp
    )

    target_link_libraries(
        nexusfs_http_metrics_tests
        PRIVATE
            nexusfs_core
            Threads::Threads
    )

    add_test(
        NAME nexusfs_http_metrics_tests
        COMMAND nexusfs_http_metrics_tests
    )

    set_tests_properties(
        nexusfs_http_metrics_tests
        PROPERTIES
            TIMEOUT 30
    )
    add_executable(
        nexusfs_metrics_endpoint_tests
        tests/test_metrics_endpoint.cpp
    )

    target_link_libraries(
        nexusfs_metrics_endpoint_tests
        PRIVATE
            nexusfs_core
            nlohmann_json::nlohmann_json
            Threads::Threads
    )

    add_test(
        NAME nexusfs_metrics_endpoint_tests
        COMMAND nexusfs_metrics_endpoint_tests
    )

    set_tests_properties(
        nexusfs_metrics_endpoint_tests
        PROPERTIES
            TIMEOUT 30
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

    target_compile_options(
        nexusfsd
        PRIVATE
            /W4
            /permissive-
    )

    if(BUILD_TESTING)
        target_compile_options(
            nexusfs_http_cluster_tests
            PRIVATE
                /W4
                /permissive-
        )

        target_compile_options(
            nexusfs_cluster_node_foundation_tests
            PRIVATE
                /W4
                /permissive-
        )

        target_compile_options(
            nexusfs_storage_recovery_tests
            PRIVATE
                /W4
                /permissive-
        )

        target_compile_options(
            nexusfs_durable_file_tests
            PRIVATE
                /W4
                /permissive-
        )
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

        target_compile_options(
            nexusfs_service_tests
            PRIVATE
                /W4
                /permissive-
        )

        target_compile_options(
            nexusfs_http_tests
            PRIVATE
                /W4
                /permissive-
        )

        target_compile_options(
            nexusfs_http_upload_tests
            PRIVATE
                /W4
                /permissive-
        )

        target_compile_options(
            nexusfs_daemon_configuration_tests
            PRIVATE
                /W4
                /permissive-
        )

        target_compile_options(
            nexusfs_http_server_lifecycle_tests
            PRIVATE
                /W4
                /permissive-
        )

        target_compile_options(
            nexusfs_service_concurrency_tests
            PRIVATE
                /W4
                /permissive-
        )

        target_compile_options(
            nexusfs_json_logger_tests
            PRIVATE
                /W4
                /permissive-
        )
        target_compile_options(
            nexusfs_metrics_registry_tests
            PRIVATE
                /W4
                /permissive-
        )

        target_compile_options(
            nexusfs_http_logging_tests
            PRIVATE
                /W4
                /permissive-
        )
        target_compile_options(
            nexusfs_http_metrics_tests
            PRIVATE
                /W4
                /permissive-
        )
        target_compile_options(
            nexusfs_metrics_endpoint_tests
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

    target_compile_options(
        nexusfsd
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    if(BUILD_TESTING)
        target_compile_options(
            nexusfs_http_cluster_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            nexusfs_cluster_node_foundation_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            nexusfs_storage_recovery_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            nexusfs_durable_file_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
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

        target_compile_options(
            nexusfs_service_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            nexusfs_http_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            nexusfs_http_upload_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            nexusfs_daemon_configuration_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            nexusfs_http_server_lifecycle_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            nexusfs_service_concurrency_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            nexusfs_json_logger_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
        target_compile_options(
            nexusfs_metrics_registry_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        target_compile_options(
            nexusfs_http_logging_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
        target_compile_options(
            nexusfs_http_metrics_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
        target_compile_options(
            nexusfs_metrics_endpoint_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()
endif()