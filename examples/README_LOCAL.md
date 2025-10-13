# Local Connection Examples

These examples demonstrate using EntropyNetworking's local connection API with Unix domain sockets for high-performance inter-process communication (IPC).

## Overview

The local connection examples show:
- Platform-agnostic local connection API (`openLocalConnection()`)
- Unix socket communication on Linux/macOS
- Message framing with 4-byte length headers
- Connection state management
- Statistics tracking (bytes, messages, connection time)
- Proper connection lifecycle (connect, send/receive, disconnect)

## Files

- `local_server.cpp` - Unix socket server that accepts connections and echoes messages
- `local_client.cpp` - Client using ConnectionManager's platform-agnostic API

## Building

The examples are built automatically with the main project:

```bash
cd build
cmake ..
cmake --build .
```

Executables are created in `build/examples/`:
- `entropy_local_server`
- `entropy_local_client`

## Running the Examples

### Start the server:

```bash
cd build/examples
./entropy_local_server
```

Output:
```
Starting EntropyNetworking Local Server
Socket path: /tmp/entropy_local.sock
Server listening on /tmp/entropy_local.sock
Waiting for client connection...
```

### In another terminal, run the client:

```bash
cd build/examples
./entropy_local_client
```

Output:
```
Starting EntropyNetworking Local Client
Connecting to: /tmp/entropy_local.sock
Connection created, state: 0
Connecting...
Connected! State: 2
Received message: Welcome to Entropy Local Server!
Sending: Hello from local client!
Received message: Echo: Hello from local client!
...

Connection Statistics:
  Bytes sent: 120
  Bytes received: 180
  Messages sent: 4
  Messages received: 5
  Messages processed: 5
  Connection time: 1760387528683 ms since epoch
```

## Key Features Demonstrated

### Platform-Agnostic API

The client uses `ConnectionManager::openLocalConnection()` which automatically selects the appropriate backend:

```cpp
ConnectionManager connMgr(64);
auto conn = connMgr.openLocalConnection("/tmp/entropy_local.sock");
```

This will use:
- **Unix sockets** on Linux/macOS
- **Named pipes** on Windows (when implemented)
- **XPC** on macOS (when configured)

### Connection State Management

```cpp
// Connect
auto result = conn.connect();

// Wait for connection
while (!conn.isConnected() && attempts < 50) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    attempts++;
}

// Check state
ConnectionState state = conn.getState();
```

### Message Callbacks

```cpp
auto* rawConn = connMgr.getConnectionPointer(conn);
rawConn->setMessageCallback([](const std::vector<uint8_t>& data) {
    std::string message(data.begin(), data.end());
    std::cout << "Received: " << message << std::endl;
});
```

### Sending Data

```cpp
std::vector<uint8_t> data(message.begin(), message.end());
auto result = conn.send(data);  // Reliable delivery
```

### Statistics

```cpp
auto stats = conn.getStats();
std::cout << "Bytes sent: " << stats.bytesSent << std::endl;
std::cout << "Messages sent: " << stats.messagesSent << std::endl;
std::cout << "Connection time: " << stats.connectTime << std::endl;
```

## Protocol Details

Messages use a simple frame-based protocol:
- **4-byte length header** (network byte order)
- **Variable-length payload** (up to 16MB)

Example:
```
[0x00][0x00][0x00][0x0C] "Hello World!"
 ^                        ^
 |                        |
 Length (12 bytes)        Payload
```

This ensures reliable message boundaries over the stream-based Unix socket.

## Performance

Unix sockets provide excellent performance for local IPC:
- **Low latency**: No network stack overhead
- **High throughput**: Direct memory-to-memory transfer
- **Zero-copy**: Modern kernels optimize Unix socket I/O
- **Reliable**: Stream-based, ordered delivery

Typical use cases:
- Local microservices communication
- Process coordination
- Signaling servers
- Local game server/client testing

## Comparison with Remote Connections

| Feature | Local (Unix Socket) | Remote (WebRTC) |
|---------|---------------------|-----------------|
| Latency | <1μs | 10-100ms |
| Throughput | GB/s | MB/s |
| Setup | Instant | Requires signaling |
| Use Case | Same machine | Internet/LAN |
| Reliability | Guaranteed | Subject to network |

## Next Steps

For production applications:
1. Add error handling and reconnection logic
2. Implement application-level protocol (see `protocol_server.cpp`)
3. Use SessionManager for entity synchronization
4. Add authentication/authorization if needed

See the protocol examples for more advanced usage with EntityCreated/EntityDestroyed messages.
