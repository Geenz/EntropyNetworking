/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include <cstdint>
#include <chrono>

namespace EntropyEngine {
namespace Networking {

/**
 * @brief Get current timestamp in microseconds since epoch
 *
 * Returns the current system time as microseconds since the Unix epoch.
 * This is the standard timestamp format used throughout the networking
 * protocol for message batches and timing information.
 *
 * @return Timestamp in microseconds since Unix epoch
 */
inline uint64_t getCurrentTimestampMicros() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace Networking
} // namespace EntropyEngine
