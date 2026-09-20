---
title: WebRTC (Remote)
sidebar:
  order: 3
---

# WebRTC (Remote Networking)

Designed for internet-based connectivity (e.g., Portal to remote Server).

## WebRTCConnection
*   **Use Case**: Client-Server communication over the public internet.
*   **Mechanism**: [libdatachannel](https://github.com/paullouisageneau/libdatachannel) (C++ WebRTC implementation).
*   **Features**:
    *   **NAT Traversal**: ICE/STUN/TURN support.
    *   **Security**: DTLS encryption enforced.
    *   **Unreliable Channels**: True UDP-based unreliable delivery for high-frequency simulation data.
    *   **Multiplexing**: Multiple data channels over a single peer connection.
