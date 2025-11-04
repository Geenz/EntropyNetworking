# Test configuration for EntropyNetworking

find_package(GTest CONFIG REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)

# Test executable
add_executable(EntropyNetworkingTests
    PropertyHashTests.cpp
    PropertyRegistryTests.cpp
    PropertyTypesTests.cpp
    ComponentSchemaTests.cpp
    ComponentSchemaRegistryTests.cpp
    SchemaGenerationTests.cpp
    SchemaNackTrackerTests.cpp
    ConnectionManagerTests.cpp
    WebRTCConnectionTests.cpp
    WebRTCIntegrationTests.cpp
    LocalIpcTests.cpp
    CallbackFanoutTests.cpp
    ConnectionManagerLifecycleTests.cpp
    ManagerMetricsTests.cpp
    NetworkSessionNackTests.cpp
    NetworkSessionHandshakeTests.cpp
    # WebDAVConnectionTests.cpp - REMOVED: WebDAVConnection no longer exists
    WebDAVPropfindParserTests.cpp
    # WebDAVBackendTests.cpp - REMOVED: Used mock WebDAVConnection
    WebDavAdapterTests.cpp
    # WebDAVStreamingTests.cpp - REMOVED: Used mock WebDAVConnection
    WebDAVClientIntegrationTests.cpp
    HttpClientTests.cpp
    HttpClientRobustnessTests.cpp
    HttpClientWebDAVIntegrationTest.cpp
    HttpClientStreamingTests.cpp
)

# Tests link libraries

target_link_libraries(EntropyNetworkingTests
    PRIVATE
        EntropyNetworking
        GTest::gtest
        GTest::gtest_main
        tinyxml2::tinyxml2
)

# Link Winsock on Windows for test-local raw socket server
if (WIN32)
    target_link_libraries(EntropyNetworkingTests PRIVATE ws2_32)
endif()

# Enable testing
include(GoogleTest)
gtest_discover_tests(EntropyNetworkingTests)


# Fuzzing target (optional)
option(ENABLE_FUZZING "Build fuzzing targets (requires Clang/libFuzzer)" OFF)

if(ENABLE_FUZZING)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_executable(entropy_unix_frame_fuzz
            FuzzUnixFrame.cpp
        )
        # Do not add heavy dependencies; fuzz harness is standalone
        target_compile_options(entropy_unix_frame_fuzz PRIVATE -O1)
        # Link flags will be provided by CI; keep target simple here
        set_target_properties(entropy_unix_frame_fuzz PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
    else()
        message(WARNING "ENABLE_FUZZING is ON but compiler is not Clang; skipping fuzz target.")
    endif()
endif()
