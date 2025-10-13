cmake_minimum_required(VERSION 3.20)

project(EntropyNetworking VERSION 0.1.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find dependencies
find_package(EntropyCore CONFIG REQUIRED)
find_package(CapnProto CONFIG REQUIRED)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(zstd CONFIG REQUIRED)

# Generate Cap'n Proto code
set(CAPNP_SCHEMA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/Networking/Protocol/entropy.capnp")
capnp_generate_cpp(CAPNP_SRCS CAPNP_HDRS ${CAPNP_SCHEMA_FILE})

# Source files
set(NETWORKING_SOURCES
    src/Networking/Core/PropertyHash.cpp
    src/Networking/Core/PropertyRegistry.cpp
    src/Networking/Core/PropertyTypes.cpp
    src/Networking/Protocol/MessageSerializer.cpp
    src/Networking/Transport/UnixSocketConnection.cpp
    src/Networking/Transport/UnixSocketServer.cpp
    src/Networking/Transport/WebRTCConnection.cpp
    src/Networking/Transport/ConnectionManager.cpp
    src/Networking/Transport/ConnectionHandle.cpp
    src/Networking/Session/NetworkSession.cpp
    src/Networking/Session/SessionHandle.cpp
    src/Networking/Session/SessionManager.cpp
    src/Networking/Session/BatchManager.cpp
    ${CAPNP_SRCS}
)

# Add platform-specific sources
if(APPLE)
    list(APPEND NETWORKING_SOURCES
        src/Networking/Transport/XPCConnection.cpp
        src/Networking/Transport/XPCServer.cpp
    )
endif()

# Create library
add_library(EntropyNetworking ${NETWORKING_SOURCES})

# Apple-specific compile options (required for XPC/Blocks)
if(APPLE)
    target_compile_options(EntropyNetworking PRIVATE -fblocks)
endif()

target_link_libraries(EntropyNetworking
    PUBLIC
        EntropyCore::Core
        CapnProto::capnp
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        LibDataChannel::LibDataChannel
        $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>
)

target_include_directories(EntropyNetworking
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
)

# Installation rules
install(TARGETS EntropyNetworking
    EXPORT EntropyNetworkingTargets
)

install(DIRECTORY include/
    DESTINATION include
)

install(EXPORT EntropyNetworkingTargets
    FILE EntropyNetworkingTargets.cmake
    NAMESPACE EntropyNetworking::
    DESTINATION lib/cmake/EntropyNetworking
)

# Create config file
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/EntropyNetworkingConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EntropyNetworkingConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/EntropyNetworkingConfig.cmake"
    INSTALL_DESTINATION lib/cmake/EntropyNetworking
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/EntropyNetworkingConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/EntropyNetworkingConfigVersion.cmake"
    DESTINATION lib/cmake/EntropyNetworking
)

# Testing
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
option(BUILD_EXAMPLES "Build example applications" ON)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

