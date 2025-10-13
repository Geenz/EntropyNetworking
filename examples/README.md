# EntropyNetworking Examples

Simple examples demonstrating WebRTC data channel communication using libdatachannel.

## Building

```bash
cd /path/to/EntropyNetworking
cmake -B build
cmake --build build
```

## Running the Examples

### Server

Start the server first:

```bash
cd build/examples
./entropy_server
```

The server will:
- Start a WebSocket signaling server on port 8080
- Wait for client connections
- Receive WebRTC data channel connections
- Exchange messages with connected clients

### Client

In a separate terminal, start the client:

```bash
cd build/examples
./entropy_client
```

The client will:
- Connect to the signaling server at `ws://localhost:8080`
- Establish a WebRTC peer connection
- Create a data channel named "entropy-data"
- Send "Hello from client!" message
- Receive responses from the server

## Example Output

**Server:**
```
Starting EntropyNetworking Server Example
WebSocket signaling server listening on port 8080
Server ready. Waiting for client connection...
Client connected to signaling server
Received SDP offer from client
Sending answer to client
Sending ICE candidate to client
Received ICE candidate from client
Data channel 'entropy-data' received from client
Data channel open on server
Sending hello from server
Server received message: Hello from client!
```

**Client:**
```
Starting EntropyNetworking Client Example
Connecting to signaling server at ws://localhost:8080
Client ready. Press Ctrl+C to quit
Connected to signaling server
Sending offer to server
Sending ICE candidate to server
Received SDP answer from server
Received ICE candidate from server
Data channel open, sending hello from client
Client received message: Hello from server!
Client received message: Server got your message!
```

## How It Works

### 1. Bootstrap Phase (WebSocket Signaling)

The initial connection uses WebSocket for signaling:
- Client connects to server's WebSocket on port 8080
- Client creates WebRTC peer connection and data channel
- Client generates SDP offer → sends via WebSocket
- Server generates SDP answer → sends via WebSocket
- Both exchange ICE candidates via WebSocket

### 2. Operational Phase (WebRTC Data Channels)

Once the WebRTC connection is established:
- All application messages flow through the data channel
- No more WebSocket communication needed
- Direct peer-to-peer data channel communication

This demonstrates the core pattern:
- **WebSocket**: Bootstrap only (SDP/ICE exchange)
- **WebRTC Data Channels**: All application protocol communication

## Notes

- These examples use localhost connections (no STUN/TURN needed)
- For remote connections, configure ICE servers in `rtc::Configuration`
- The signaling mechanism (WebSocket) is only used during connection establishment
- Once established, 100% of application traffic goes through WebRTC data channels
