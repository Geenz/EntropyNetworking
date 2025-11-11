# Example applications for EntropyNetworking

find_package(LibDataChannel CONFIG REQUIRED)

# Local connection examples (using Unix sockets)
add_executable(entropy_local_server local_server.cpp)
target_link_libraries(entropy_local_server PRIVATE EntropyNetworking EntropyCore::Core)

add_executable(entropy_local_client local_client.cpp)
target_link_libraries(entropy_local_client PRIVATE EntropyNetworking EntropyCore::Core)

# Canvas examples (demonstrating complete protocol stack with schemas)
add_executable(entropy_canvas_server canvas_server.cpp)
target_link_libraries(entropy_canvas_server PRIVATE EntropyNetworking LibDataChannel::LibDataChannel EntropyCore::Core)

add_executable(entropy_canvas_client canvas_client.cpp)
target_link_libraries(entropy_canvas_client PRIVATE EntropyNetworking LibDataChannel::LibDataChannel EntropyCore::Core)

# Schema generation example (auto-generate ComponentSchema from reflection)
add_executable(entropy_schema_generation schema_generation.cpp)
target_link_libraries(entropy_schema_generation PRIVATE EntropyNetworking EntropyCore::Core)
