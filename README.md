# Entropy Networking

A high-performance, cross-platform networking library for the Entropy Engine. Provides a unified, handle-based API for both local IPC and remote connectivity with lock-free operation and generation-stamped handles.

## Features

### Transport Layer
- **Local IPC**
  - Unix domain sockets (Linux/macOS) with configurable framing and backpressure
  - XPC connections (Apple platforms: iOS/tvOS/watchOS/visionOS)
  - Automatic platform selection via `openLocalConnection()`
- **Remote Connectivity**
  - WebRTC with reliable and unreliable data channels
  - Cross-platform support for peer-to-peer and client-server architectures

### Session Layer
- **Protocol-Level Messaging**
  - Entity creation/destruction synchronization
  - Property update batching with configurable rates
  - Scene snapshot support for full state transfer
  - Hash-based property registry for type validation

### Core Architecture
- **WorkContractGroup Pattern**: Slot-based managers with generation-stamped handles
- **Lock-Free Design**: Atomic operations and minimal per-slot locking
- **Thread-Safe Callbacks**: Lock-free fanout using atomic `shared_ptr`
- **Comprehensive Observability**: Per-connection stats and aggregate metrics

## Quick Start

### Local IPC (Unix Socket)

```cpp
using namespace EntropyEngine::Networking;

// Server
ConnectionManager serverMgr(8);
auto server = createLocalServer(&serverMgr, "/tmp/entropy_demo.sock");
server->listen();

std::thread acceptThread([&]{
    auto conn = server->accept();
    if (!conn.valid()) return;

    conn.setMessageCallback([conn](const std::vector<uint8_t>& data) mutable noexcept {
        std::string msg(data.begin(), data.end());
        std::cout << "Received: " << msg << std::endl;
        (void)conn.send(data); // Echo back
    });

    // Keep connection alive
    while (conn.isConnected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
});

// Client
ConnectionManager clientMgr(8);
auto client = clientMgr.openLocalConnection("/tmp/entropy_demo.sock");

client.setStateCallback([](ConnectionState s) noexcept {
    std::cout << "State changed: " << static_cast<int>(s) << std::endl;
});

auto r = client.connect();
if (r.success() && client.isConnected()) {
    std::vector<uint8_t> hello{'H','e','l','l','o'};
    client.send(hello);
}
```

### Protocol-Level Session

```cpp
using namespace EntropyEngine::Networking;

ConnectionManager connMgr(1024);
SessionManager sessMgr(&connMgr, 512);

// Create connection
auto conn = connMgr.openLocalConnection("/tmp/entropy.sock");
conn.connect();

// Create session
auto session = sessMgr.createSession(conn);

// Send entity created
session.sendEntityCreated(
    42,              // entityId
    "MyApp",         // appId
    "Player",        // typeName
    0                // parentId (0 = root)
);

// Send property update
PropertyValue position;
position.type = PropertyType::Vec3;
position.vec3 = {1.0f, 2.0f, 3.0f};
session.sendPropertyUpdate(42, "transform.position", position);

// Access property registry
auto& registry = session.getPropertyRegistry();
```

### Property Batching

```cpp
using namespace EntropyEngine::Networking;

// Create batch manager
auto batcher = std::make_shared<BatchManager>(session, 16); // 16ms = 60Hz

// Hash is computed once per property
auto posHash = computePropertyHash(42, "MyApp", "Player", "transform.position");

// Update properties (batched automatically)
PropertyValue newPos;
newPos.type = PropertyType::Vec3;
newPos.vec3 = {5.0f, 10.0f, 15.0f};
batcher->updateProperty(posHash, PropertyType::Vec3, newPos);

// Application calls processBatch() periodically (e.g., in game loop)
batcher->processBatch(); // Sends batch over unreliable channel
```

## Configuration

### Connection Settings

```cpp
ConnectionConfig cfg;
cfg.connectTimeoutMs = 3000;       // Connection timeout
cfg.sendPollTimeoutMs = 100;       // Poll timeout for blocking send (Unix)
cfg.sendMaxPolls = 20;             // Max polls before timeout (Unix)
cfg.maxMessageSize = 16*1024*1024; // Message size limit (16MB)
cfg.recvIdlePollMs = 1;            // Receive loop idle poll interval

// Apply configuration
auto handle = mgr.openConnection(cfg);
```

### Server Settings

```cpp
LocalServerConfig scfg;
scfg.backlog = 64;                 // Listen backlog
scfg.acceptPollIntervalMs = 250;   // Accept loop polling interval
scfg.chmodMode = 0660;             // Socket file permissions
scfg.unlinkOnStart = true;         // Remove stale socket before bind

auto server = createLocalServer(&mgr, "/tmp/entropy.sock", scfg);
```

## Backpressure and Flow Control

### Blocking Send
```cpp
// send() handles backpressure with EAGAIN polling (Unix) or buffering (WebRTC)
auto result = conn.send(largeData);
if (result.failed()) {
    // Handle error (timeout, connection closed, etc.)
}
```

### Non-Blocking Send
```cpp
// trySend() returns immediately with WouldBlock if transport is busy
auto result = conn.trySend(data);
if (result.error == NetworkError::WouldBlock) {
    // Apply application-level backpressure
    // Queue data or slow down production
}
```

## Observability

### Per-Connection Statistics

```cpp
auto stats = conn.getStats();
std::cout << "Bytes sent: " << stats.bytesSent << std::endl;
std::cout << "Bytes received: " << stats.bytesReceived << std::endl;
std::cout << "Messages sent: " << stats.messagesSent << std::endl;
std::cout << "Connect time: " << stats.connectTime.count() << "ms" << std::endl;
std::cout << "Last activity: " << stats.lastActivityTime.count() << "ms ago" << std::endl;
```

### Aggregate Metrics

```cpp
auto metrics = mgr.getManagerMetrics();
std::cout << "Total connections opened: " << metrics.connectionsOpened << std::endl;
std::cout << "Total bytes sent: " << metrics.totalBytesSent << std::endl;
std::cout << "WouldBlock count: " << metrics.wouldBlockSends << std::endl;
```

### Logging

The library uses EntropyCore logging macros. Set log level in your application:

```cpp
// In your main.cpp or initialization code
EntropyCore::setLogLevel(EntropyCore::LogLevel::Debug);
```

## State Management and Callbacks

### State Callbacks

```cpp
conn.setStateCallback([](ConnectionState state) noexcept {
    switch (state) {
        case ConnectionState::Disconnected:
            std::cout << "Disconnected" << std::endl;
            break;
        case ConnectionState::Connecting:
            std::cout << "Connecting..." << std::endl;
            break;
        case ConnectionState::Connected:
            std::cout << "Connected!" << std::endl;
            break;
        case ConnectionState::Failed:
            std::cout << "Connection failed" << std::endl;
            break;
    }
});
```

### Message Callbacks

```cpp
conn.setMessageCallback([](const std::vector<uint8_t>& data) noexcept {
    // Process incoming message
    // Called from receive thread - keep processing fast or queue for later
});
```

### Thread Safety

All public methods are thread-safe:
- Callbacks are invoked using lock-free atomic `shared_ptr` access
- State queries use atomic loads
- Internal operations use lock-free algorithms or minimal per-slot locking

## Building

### Requirements

- C++20 compiler (Clang, GCC, MSVC)
- CMake 3.20+
- vcpkg for dependency management

### Dependencies (via vcpkg)

- EntropyCore
- Cap'n Proto (protocol serialization)
- libdatachannel (WebRTC)
- OpenSSL
- zstd (compression)

### Build Commands

```bash
# Configure
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# Build library
cmake --build build --target EntropyNetworking

# Build and run tests
cmake --build build --target EntropyNetworkingTests
ctest --test-dir build

# Build examples
cmake --build build --target entropy_local_server
cmake --build build --target entropy_local_client
cmake --build build --target entropy_protocol_server
cmake --build build --target entropy_protocol_client
cmake --build build --target entropy_batch_demo_server
cmake --build build --target entropy_batch_demo_client
```

## Testing

The library includes comprehensive test coverage:

- **PropertyHashTests** - Property hash generation and collision detection
- **PropertyRegistryTests** - Registry operations and thread safety
- **PropertyTypesTests** - Property value serialization
- **ConnectionManagerTests** - Connection lifecycle and handle validation
- **ConnectionManagerLifecycleTests** - Slot reuse and generation tracking
- **ManagerMetricsTests** - Metrics deduplication and accuracy
- **CallbackFanoutTests** - Callback orchestration and timing
- **LocalIpcTests** - Unix socket server/client integration
- **WebRTCConnectionTests** - WebRTC functionality (requires libdatachannel)

Run all tests:
```bash
cd build
ctest --output-on-failure
```

## Examples

### Available Examples

- **entropy_local_server** / **entropy_local_client** - Basic Unix socket echo server/client
- **entropy_protocol_server** / **entropy_protocol_client** - Entity/property protocol demo
- **entropy_batch_demo_server** / **entropy_batch_demo_client** - Property batching demo
- **entropy_server** / **entropy_client** - Legacy examples (deprecated)

### Running Examples

```bash
# Terminal 1: Start server
./build/examples/entropy_local_server

# Terminal 2: Run client
./build/examples/entropy_local_client
```

See `examples/README.md` for detailed example documentation.

## Architecture

### Handle/Manager Pattern

```
ConnectionManager (WorkContractGroup)
├── Owns connection slots with generation counters
├── Returns generation-stamped ConnectionHandle objects
└── Validates handles on every operation

SessionManager (WorkContractGroup)
├── Builds on ConnectionManager
├── Returns generation-stamped SessionHandle objects
└── Provides protocol-level operations
```

### Layered Design

```
┌─────────────────────────────────────┐
│   Session Layer (Protocol)          │
│   SessionManager, SessionHandle      │
│   NetworkSession, BatchManager       │
├─────────────────────────────────────┤
│   Transport Layer (Connections)      │
│   ConnectionManager, ConnectionHandle│
│   NetworkConnection implementations  │
├─────────────────────────────────────┤
│   Core (Serialization & Registry)    │
│   PropertyRegistry, MessageSerializer│
│   PropertyHash, PropertyTypes        │
└─────────────────────────────────────┘
```

### Backend Implementations

- **UnixSocketConnection** - Unix domain socket with framed messaging
- **XPCConnection** - Apple XPC for iOS/macOS IPC
- **WebRTCConnection** - libdatachannel-based P2P connectivity

## API Reference

All APIs are documented inline in the header files following Doxygen conventions. Key headers:

- **Transport Layer**: `src/Networking/Transport/ConnectionManager.h`, `ConnectionHandle.h`
- **Session Layer**: `src/Networking/Session/SessionManager.h`, `SessionHandle.h`
- **Protocol**: `src/Networking/Core/PropertyRegistry.h`, `PropertyTypes.h`
- **Serialization**: `src/Networking/Protocol/MessageSerializer.h`

For detailed examples, see `examples/README.md`.

For documentation **guidelines** (if contributing), see `DOCUMENTATION.md`.

## License

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.

Copyright (c) 2025 Jonathan "Geenz" Goodman

## Contributing

This library is part of the Entropy Engine project. Contributions should follow:
- C++20 modern best practices
- Lock-free designs where possible
- Comprehensive testing (maintain test coverage)
- Documentation standards (see `DOCUMENTATION.md`)

