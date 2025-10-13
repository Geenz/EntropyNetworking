# EntropyNetworking

Networking library for the Entropy Engine.

## Building

This project uses CMake and vcpkg for dependency management.

### Prerequisites

- CMake 3.20 or later
- vcpkg
- C++20 compatible compiler

### Build Instructions

```bash
# Configure
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[path to vcpkg]/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build

# Install (optional)
cmake --install build --prefix [install location]
```

## Dependencies

- **EntropyCore** - Core utilities and concurrency primitives from the Entropy custom registry

## Project Structure

```
EntropyNetworking/
├── cmake/                      # CMake configuration files
├── include/Entropy/Networking/ # Public headers
├── CMakeLists.txt             # Main build configuration
├── vcpkg.json                 # Package dependencies
├── vcpkg-configuration.json   # Custom registry configuration
├── CODESTYLE.md              # Code style guide
└── DOCUMENTATION.md          # Documentation standards
```

## Usage

```cpp
#include <Entropy/Networking/YourHeader.h>

// Your code here
```

In your CMakeLists.txt:
```cmake
find_package(EntropyNetworking CONFIG REQUIRED)
target_link_libraries(YourTarget PRIVATE EntropyNetworking::EntropyNetworking)
```
