/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file ErrorCodes.h
 * @brief Error handling types for Entropy networking
 *
 * Defines error codes and result types used throughout the networking system.
 */

#pragma once

#include <optional>
#include <string>
#include <stdexcept>

namespace EntropyEngine {
namespace Networking {

/**
 * @brief Error codes for networking operations
 */
enum class NetworkError {
    None,                       ///< No error
    HashCollision,              ///< Property hash collision detected
    UnknownProperty,            ///< Property hash not found in registry
    TypeMismatch,               ///< Property value type doesn't match expected type
    InvalidMessage,             ///< Malformed or invalid message
    SerializationFailed,        ///< Failed to serialize message
    DeserializationFailed,      ///< Failed to deserialize message
    CompressionFailed,          ///< Failed to compress data
    DecompressionFailed,        ///< Failed to decompress data
    ConnectionClosed,           ///< Connection was closed
    Timeout,                    ///< Operation timed out
    InvalidParameter,           ///< Invalid parameter provided
    RegistryFull,               ///< Registry has reached capacity limit
    EntityNotFound,             ///< Entity ID not found
    AlreadyExists,              ///< Entity or property already exists
    WouldBlock,                 ///< Operation would block (non-blocking backpressure)
    ResourceLimitExceeded,      ///< Resource limit exceeded (entities, properties, etc.)
    HandshakeFailed             ///< Handshake protocol failed
};

/**
 * @brief Convert error code to human-readable string
 * @param error The error code
 * @return Description of the error
 */
inline const char* errorToString(NetworkError error) {
    switch (error) {
        case NetworkError::None: return "No error";
        case NetworkError::HashCollision: return "Property hash collision";
        case NetworkError::UnknownProperty: return "Unknown property";
        case NetworkError::TypeMismatch: return "Type mismatch";
        case NetworkError::InvalidMessage: return "Invalid message";
        case NetworkError::SerializationFailed: return "Serialization failed";
        case NetworkError::DeserializationFailed: return "Deserialization failed";
        case NetworkError::CompressionFailed: return "Compression failed";
        case NetworkError::DecompressionFailed: return "Decompression failed";
        case NetworkError::ConnectionClosed: return "Connection closed";
        case NetworkError::Timeout: return "Timeout";
        case NetworkError::InvalidParameter: return "Invalid parameter";
        case NetworkError::RegistryFull: return "Registry full";
        case NetworkError::EntityNotFound: return "Entity not found";
        case NetworkError::AlreadyExists: return "Already exists";
        case NetworkError::WouldBlock: return "Would block";
        case NetworkError::ResourceLimitExceeded: return "Resource limit exceeded";
        case NetworkError::HandshakeFailed: return "Handshake failed";
        default: return "Unknown error";
    }
}

/**
 * @brief Result type for operations that may fail
 *
 * Encapsulates a value and an error code. Check success() before accessing value.
 *
 * @code
 * auto result = someOperation();
 * if (result.success()) {
 *     useValue(result.value);
 * } else {
 *     handleError(result.error);
 * }
 * @endcode
 */
template<typename T>
struct Result {
    T value;                    ///< Result value (valid only if error == None)
    NetworkError error;         ///< Error code
    std::string errorMessage;   ///< Optional detailed error message

    /**
     * @brief Check if operation succeeded
     * @return true if no error occurred
     */
    bool success() const {
        return error == NetworkError::None;
    }

    /**
     * @brief Check if operation failed
     * @return true if an error occurred
     */
    bool failed() const {
        return error != NetworkError::None;
    }

    /**
     * @brief Get the value or throw on error
     * @return The contained value
     * @throws std::runtime_error if operation failed
     */
    T& valueOrThrow() {
        if (failed()) {
            throw std::runtime_error(errorMessage.empty() ?
                errorToString(error) : errorMessage);
        }
        return value;
    }

    /**
     * @brief Create a successful result
     * @param val The result value
     * @return Result with no error
     */
    static Result<T> ok(T val) {
        return Result<T>{std::move(val), NetworkError::None, ""};
    }

    /**
     * @brief Create a failed result
     * @param err The error code
     * @param message Optional error message
     * @return Result with error
     */
    static Result<T> err(NetworkError err, std::string message = "") {
        return Result<T>{T{}, err, std::move(message)};
    }
};

/**
 * @brief Result specialization for void operations
 */
template<>
struct Result<void> {
    NetworkError error;
    std::string errorMessage;

    bool success() const { return error == NetworkError::None; }
    bool failed() const { return error != NetworkError::None; }

    void throwOnError() const {
        if (failed()) {
            throw std::runtime_error(errorMessage.empty() ?
                errorToString(error) : errorMessage);
        }
    }

    static Result<void> ok() {
        return Result<void>{NetworkError::None, ""};
    }

    static Result<void> err(NetworkError err, std::string message = "") {
        return Result<void>{err, std::move(message)};
    }
};

} // namespace Networking
} // namespace EntropyEngine
