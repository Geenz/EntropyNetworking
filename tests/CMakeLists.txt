# Test configuration for EntropyNetworking

find_package(GTest CONFIG REQUIRED)

# Test executable
add_executable(EntropyNetworkingTests
    PropertyHashTests.cpp
    PropertyRegistryTests.cpp
    PropertyTypesTests.cpp
    ConnectionManagerTests.cpp
    WebRTCConnectionTests.cpp
    WebRTCIntegrationTests.cpp
    LocalIpcTests.cpp
    CallbackFanoutTests.cpp
    ConnectionManagerLifecycleTests.cpp
    ManagerMetricsTests.cpp
    WebDAVConnectionTests.cpp
    WebDAVPropfindParserTests.cpp
    WebDAVBackendTests.cpp
    WebDavAdapterTests.cpp
)

target_link_libraries(EntropyNetworkingTests
    PRIVATE
        EntropyNetworking
        GTest::gtest
        GTest::gtest_main
)

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
