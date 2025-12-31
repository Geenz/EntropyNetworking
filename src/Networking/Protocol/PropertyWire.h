/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

#pragma once

#include "../Core/PropertyHash.h"
#include "entropy.capnp.h"

namespace EntropyEngine::Networking::Wire
{

/**
 * @brief Convert PropertyHash to Cap'n Proto PropertyHash128 builder
 * @param b Cap'n Proto builder
 * @param h PropertyHash to convert
 */
inline void toCapnp(::PropertyHash128::Builder b, const PropertyHash& h) {
    b.setHigh(h.high);
    b.setLow(h.low);
}

/**
 * @brief Convert Cap'n Proto PropertyHash128 reader to PropertyHash
 * @param r Cap'n Proto reader
 * @return PropertyHash
 */
inline PropertyHash fromCapnp(::PropertyHash128::Reader r) {
    return PropertyHash{r.getHigh(), r.getLow()};
}

}  // namespace EntropyEngine::Networking::Wire
