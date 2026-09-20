---
title: IPC (Local)
sidebar:
  order: 2
---

# Local IPC

Designed for ultra-low latency communication between local applications (e.g., Canvas and Paint).

## SharedMemoryConnection
*   **Use Case**: High-frequency data (e.g., continuous property updates, large texture uploads).
*   **Mechanism**: Ring buffers in memory-mapped files.
*   **Performance**: Zero-copy (writes directly to shared region).
*   **Synchronization**: Platform-specific (Futex on Linux, Ulock on macOS).
*   **Architecture**: Similar to Wayland's `wl_shm`.

## UnixSocketConnection (macOS/Linux)
*   **Use Case**: Reliable command channel, control messages.
*   **Mechanism**: `AF_UNIX` domain sockets.
*   **Features**: Ordered delivery, FD passing (future), widely supported.

## NamedPipeConnection (Windows)
*   **Use Case**: Windows equivalent of Unix Sockets.
*   **Mechanism**: Windows Named Pipes (`CreateNamedPipe`).
*   **Features**: Overlapped I/O for async operations.
