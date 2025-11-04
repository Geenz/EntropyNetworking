# Example applications for EntropyNetworking

find_package(LibDataChannel CONFIG REQUIRED)

# Basic WebRTC examples (hello world)
add_executable(entropy_server server.cpp)
target_link_libraries(entropy_server PRIVATE LibDataChannel::LibDataChannel)

add_executable(entropy_client client.cpp)
target_link_libraries(entropy_client PRIVATE LibDataChannel::LibDataChannel)

# Protocol-aware examples (using NetworkSession)
add_executable(entropy_protocol_server protocol_server.cpp)
target_link_libraries(entropy_protocol_server PRIVATE EntropyNetworking LibDataChannel::LibDataChannel)

add_executable(entropy_protocol_client protocol_client.cpp)
target_link_libraries(entropy_protocol_client PRIVATE EntropyNetworking LibDataChannel::LibDataChannel)

# Batch manager demo (using WorkContracts)
add_executable(entropy_batch_demo_server batch_demo_server.cpp)
target_link_libraries(entropy_batch_demo_server PRIVATE EntropyNetworking LibDataChannel::LibDataChannel EntropyCore::Core)

add_executable(entropy_batch_demo_client batch_demo_client.cpp)
target_link_libraries(entropy_batch_demo_client PRIVATE EntropyNetworking LibDataChannel::LibDataChannel EntropyCore::Core)

# Local connection examples (using Unix sockets)
add_executable(entropy_local_server local_server.cpp)
target_link_libraries(entropy_local_server PRIVATE EntropyNetworking EntropyCore::Core)

add_executable(entropy_local_client local_client.cpp)
target_link_libraries(entropy_local_client PRIVATE EntropyNetworking EntropyCore::Core)

# Schema generation example (auto-generate ComponentSchema from reflection)
add_executable(entropy_schema_generation schema_generation.cpp)
target_link_libraries(entropy_schema_generation PRIVATE EntropyNetworking EntropyCore::Core)
