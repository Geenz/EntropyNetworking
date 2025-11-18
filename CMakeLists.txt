cmake_minimum_required(VERSION 3.28)

project(EntropyNetworking
    VERSION 0.1.0
    DESCRIPTION "Networking library for Entropy Engine with Cap'n Proto and WebRTC support"
    LANGUAGES CXX
)

# Options
# EntropyNetworking MUST ALWAYS be built as a static library - shared linking is not supported
set(BUILD_SHARED_LIBS OFF CACHE BOOL "EntropyNetworking only supports static linking" FORCE)
option(BUILD_TESTS "Enable/build tests for EntropyNetworking" ON)
option(BUILD_EXAMPLES "Build EntropyNetworking example executables" ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" ON)

# Set C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Configure sanitizers
if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # ThreadSanitizer is mutually exclusive with AddressSanitizer
        # To use TSAN instead: cmake -DENABLE_TSAN=ON
        if(ENABLE_TSAN)
            set(SANITIZER_FLAGS "-fsanitize=thread -fno-omit-frame-pointer -g")
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
            message(STATUS "ThreadSanitizer (TSAN) enabled")
        else()
            set(SANITIZER_FLAGS "-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -g")
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
            message(STATUS "Sanitizers enabled: AddressSanitizer (ASAN) and UndefinedBehaviorSanitizer (UBSAN)")
        endif()
    else()
        message(WARNING "Sanitizers requested but not supported by ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

# Configure vcpkg
if(DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# Find dependencies
find_package(EntropyCore CONFIG REQUIRED)
find_package(CapnProto CONFIG REQUIRED)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)
find_package(llhttp CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

# Only require GoogleTest when building tests
if(BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

# Generate Cap'n Proto code
set(CAPNP_SCHEMA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/Networking/Protocol/entropy.capnp")
capnp_generate_cpp(CAPNP_SRCS CAPNP_HDRS ${CAPNP_SCHEMA_FILE})

# Source files
set(NETWORKING_SOURCES
    src/Networking/Core/PropertyHash.cpp
    src/Networking/Core/PropertyRegistry.cpp
    src/Networking/Core/PropertyTypes.cpp
    src/Networking/Core/ComponentSchema.cpp
    src/Networking/Core/ComponentSchemaRegistry.cpp
    src/Networking/Core/SchemaNackTracker.cpp
    src/Networking/Protocol/MessageSerializer.cpp
    src/Networking/Protocol/ComponentSchemaSerializer.cpp
    src/Networking/Protocol/WebDavAdapter.cpp
    src/Networking/Transport/WebRTCConnection.cpp
    src/Networking/Transport/WebRTCServer.cpp
    src/Networking/Transport/RemoteServer.cpp
    src/Networking/Transport/ConnectionManager.cpp
    src/Networking/Transport/ConnectionHandle.cpp
    src/Networking/Session/NetworkSession.cpp
    src/Networking/Session/SessionHandle.cpp
    src/Networking/Session/SessionManager.cpp
    src/Networking/Session/BatchManager.cpp
    src/Networking/WebDAV/WebDAVUtils.cpp
    src/Networking/WebDAV/WebDAVPropfindParser.cpp
    src/Networking/WebDAV/WebDAVReadStream.cpp
    src/Networking/WebDAV/WebDAVFileSystemBackend.cpp
    # HTTP core client
    src/Networking/HTTP/HttpTypes.h
    src/Networking/HTTP/HttpClient.h
    src/Networking/HTTP/HttpClient.cpp
    src/Networking/HTTP/Proxy.h
    src/Networking/HTTP/Proxy.cpp
    ${CAPNP_SRCS}
)

# Unix-only local IPC (Unix domain sockets)
if(UNIX OR APPLE)
    list(APPEND NETWORKING_SOURCES
        src/Networking/Transport/UnixSocketConnection.cpp
        src/Networking/Transport/UnixSocketServer.cpp
    )
endif()

# Windows local IPC (Named Pipes)
if(WIN32)
    list(APPEND NETWORKING_SOURCES
        src/Networking/Transport/NamedPipeConnection.cpp
        src/Networking/Transport/NamedPipeServer.cpp
    )
endif()

# Apple-specific XPC transport
if(APPLE)
    list(APPEND NETWORKING_SOURCES
        src/Networking/Transport/XPCConnection.cpp
        src/Networking/Transport/XPCServer.cpp
    )
endif()

# Create library (STATIC only - shared libs are not supported)
add_library(EntropyNetworking STATIC ${NETWORKING_SOURCES})

# Set target properties
target_compile_features(EntropyNetworking PUBLIC cxx_std_20)

# Apple-specific compile options (required for XPC/Blocks)
if(APPLE)
    target_compile_options(EntropyNetworking PRIVATE -fblocks)
endif()

# Include directories
target_include_directories(EntropyNetworking
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
)

# Link dependencies
target_link_libraries(EntropyNetworking
    PUBLIC
        EntropyCore::Core
        CapnProto::capnp
        glm::glm
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        LibDataChannel::LibDataChannel
        zstd::libzstd
        tinyxml2::tinyxml2
        llhttp::llhttp_static
        CURL::libcurl
)

# Link WinHTTP for system proxy/PAC detection on Windows
if (WIN32)
    target_link_libraries(EntropyNetworking PRIVATE winhttp)
endif()

# Link macOS frameworks for CFNetwork-based proxy resolution
if(APPLE)
    target_link_libraries(EntropyNetworking PRIVATE
        "-framework CoreFoundation"
        "-framework SystemConfiguration"
        "-framework CFNetwork"
    )
endif()

# Platform and compiler-specific settings
if(WIN32)
    # Settings for Windows
    target_compile_definitions(EntropyNetworking PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(EntropyNetworking PRIVATE /W4)
    endif()
else()
    # Settings for non-Windows platforms (macOS, Linux)
    target_compile_options(EntropyNetworking PRIVATE -Wall -Wextra -Wpedantic)
    # -fexperimental-library is only supported by Clang, not GCC
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(EntropyNetworking PUBLIC -fexperimental-library)
    endif()
    if(APPLE)
        # macOS specific
        target_compile_definitions(EntropyNetworking PUBLIC EntropyDarwin)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Linux specific
        target_compile_definitions(EntropyNetworking PUBLIC EntropyLinux)
    endif()
endif()

# Export include directories for dependent projects
set_target_properties(EntropyNetworking PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME Networking
)

# Examples
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Testing
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS EntropyNetworking
    EXPORT EntropyNetworkingTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY src/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Install generated Cap'n Proto headers
install(FILES ${CAPNP_HDRS}
    DESTINATION include/Networking/Protocol
)

install(EXPORT EntropyNetworkingTargets
    FILE EntropyNetworkingTargets.cmake
    NAMESPACE EntropyNetworking::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EntropyNetworking
)

# Create config file
include(CMakePackageConfigHelpers)

configure_package_config_file(
    "cmake/EntropyNetworkingConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/EntropyNetworkingConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EntropyNetworking
)

write_basic_package_version_file(
    EntropyNetworkingConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/EntropyNetworkingConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/EntropyNetworkingConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EntropyNetworking
)
