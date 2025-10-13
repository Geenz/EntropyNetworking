# Test configuration for EntropyNetworking

find_package(GTest CONFIG REQUIRED)

# Test executable
add_executable(EntropyNetworkingTests
    PropertyHashTests.cpp
    PropertyRegistryTests.cpp
    PropertyTypesTests.cpp
    ConnectionManagerTests.cpp
    WebRTCConnectionTests.cpp
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
