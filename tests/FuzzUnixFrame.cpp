/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <cstring>

// Lightweight, dependency-free fuzz target for Unix framing logic.
// It simulates parsing of length-prefixed frames: [4-byte BE length][payload]
// to ensure no crashes/UB on arbitrary inputs.

static inline uint32_t read_be32(const uint8_t* p) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
#else
    // Big endian host
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
#endif
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Treat data as a stream containing 0..N frames. Parse conservatively.
    constexpr size_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024; // 16 MB guard
    size_t off = 0;

    // Local message buffer to simulate accumulation across recv() calls
    std::vector<uint8_t> message;
    bool readingHeader = true;
    uint32_t expectedLength = 0;

    // Iterate over the buffer in small chunks to simulate fragmentation
    while (off < size) {
        size_t chunk = std::min<size_t>(size - off, (data[off] % 32) + 1); // 1..32 bytes per step
        size_t consumed = 0;
        while (consumed < chunk) {
            if (readingHeader) {
                // Need 4 bytes for header
                size_t have = message.size();
                size_t need = 4 - have;
                size_t toCopy = std::min(need, chunk - consumed);
                message.insert(message.end(), data + off + consumed, data + off + consumed + toCopy);
                consumed += toCopy;
                if (message.size() == 4) {
                    expectedLength = read_be32(message.data());
                    // Guard absurd sizes to stay bounded
                    if (expectedLength > MAX_MESSAGE_SIZE) {
                        return 0; // treat as protocol error; stop parsing this input
                    }
                    message.clear();
                    readingHeader = false;
                }
            } else {
                size_t have = message.size();
                size_t need = expectedLength > have ? (expectedLength - have) : 0;
                size_t toCopy = std::min(need, chunk - consumed);
                message.insert(message.end(), data + off + consumed, data + off + consumed + toCopy);
                consumed += toCopy;
                if (message.size() >= expectedLength) {
                    // Simulate message delivery; verify buffer size and reset
                    if (message.size() != expectedLength) {
                        // If overrun (due to corrupted header), drop
                        message.clear();
                        readingHeader = true;
                        expectedLength = 0;
                        continue;
                    }
                    // Simple checksum-like touch to keep optimizer from removing work
                    volatile uint8_t sink = 0;
                    for (uint8_t b : message) sink ^= b;
                    (void)sink;
                    message.clear();
                    readingHeader = true;
                    expectedLength = 0;
                }
            }
        }
        off += chunk;
    }
    return 0;
}
