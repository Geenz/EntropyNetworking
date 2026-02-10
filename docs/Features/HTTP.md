---
title: HTTP Client
sidebar:
  order: 4
---

# HTTP Client

EntropyNetworking provides a high-performance, asynchronous HTTP client built on top of `libcurl`. It supports HTTP/1.1 and HTTP/2, automatic connection pooling, and request multiplexing.

## Basic Usage

The `HttpClient` interface is straightforward.

```cpp
// 1. Create client (thread-safe, reusable)
HttpClient client;

// 2. Prepare request
HttpRequest req;
req.method = HttpMethod::GET;
req.url = "https://api.example.com/data";
req.headers.add("Authorization", "Bearer token");

// 3. Execute (blocking)
HttpResponse resp = client.execute(req);

if (resp.statusCode == 200) {
    // Process resp.body (std::vector<uint8_t>)
}
```

## Streaming Requests

For large files, use `executeStream` to process data incrementally without loading the entire response into memory.

```cpp
StreamOptions opts;
opts.bufferBytes = 1024 * 1024; // 1MB buffer

auto stream = client.executeStream(req, opts);

std::vector<uint8_t> buffer(4096);
while (!stream.isDone()) {
    size_t bytesRead = stream.read(buffer.data(), buffer.size());
    if (bytesRead > 0) {
        // Write to disk or process chunk
    }
}
```

## Features

*   **Connection Pooling**: Keeps connections alive to reuse SSL handshakes.
*   **Thread Safety**: A single `HttpClient` instance can be shared across multiple threads.
*   **Proxy Support**: Automatically respects `HTTP_PROXY` and `HTTPS_PROXY` environment variables.
*   **Compression**: Automatically handles GZIP/Brotli decompression.
