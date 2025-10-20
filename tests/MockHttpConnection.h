/*
 * Mock HTTP connection implementing NetworkConnection for tests.
 */
#pragma once

#include "Networking/Transport/NetworkConnection.h"
#include <queue>
#include <string>
#include <vector>
#include <mutex>
#include <optional>
#include <chrono>

namespace EntropyEngine::Networking::Tests {

class MockHttpConnection : public NetworkConnection {
public:
    struct ScriptedResponse {
        std::vector<std::string> chunks; // each string will be delivered via onMessageReceived
        std::chrono::milliseconds delayBetween{0}; // optional delay between chunks (not used in synchronous tests)
    };

    MockHttpConnection() = default;
    ~MockHttpConnection() override { shutdownCallbacks(); }

    // Enqueue a response to be delivered upon next send()
    void enqueueResponse(ScriptedResponse resp) {
        std::lock_guard<std::mutex> lk(_mx);
        _queue.push(std::move(resp));
    }

    // Access last sent request (for assertions)
    std::string lastRequest() const {
        std::lock_guard<std::mutex> lk(_mx);
        return _lastRequest;
    }

    // NetworkConnection overrides
    Result<void> connect() override { return Result<void>::ok(); }
    Result<void> disconnect() override { return Result<void>::ok(); }
    bool isConnected() const override { return true; }

    Result<void> send(const std::vector<uint8_t>& data) override {
        std::lock_guard<std::mutex> lk(_mx);
        _lastRequest.assign(reinterpret_cast<const char*>(data.data()), data.size());
        if (_queue.empty()) {
            // no response scripted: simulate timeout by doing nothing
            return Result<void>::ok();
        }
        auto resp = std::move(_queue.front()); _queue.pop();
        for (auto& chunk : resp.chunks) {
            std::vector<uint8_t> bytes(chunk.begin(), chunk.end());
            onMessageReceived(bytes);
        }
        return Result<void>::ok();
    }

    Result<void> sendUnreliable(const std::vector<uint8_t>& data) override {
        return send(data);
    }

    ConnectionState getState() const override { return ConnectionState::Connected; }
    ConnectionType getType() const override { return ConnectionType::Local; }
    ConnectionStats getStats() const override { return ConnectionStats{}; }

private:
    mutable std::mutex _mx;
    std::queue<ScriptedResponse> _queue;
    std::string _lastRequest;
};

} // namespace EntropyEngine::Networking::Tests
