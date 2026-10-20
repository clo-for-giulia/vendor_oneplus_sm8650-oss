/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace android {
namespace fingerprint {

// Little-endian field access for the TA's flat command/response buffers. The
// span overloads ignore out-of-range accesses, the pointer overloads are for
// offsets the protocol fixes.

inline uint32_t readU32(std::span<const uint8_t> buffer, size_t offset) {
    uint32_t value = 0;
    if (offset + sizeof(value) <= buffer.size()) {
        memcpy(&value, buffer.data() + offset, sizeof(value));
    }
    return value;
}

inline int32_t readI32(std::span<const uint8_t> buffer, size_t offset) {
    return static_cast<int32_t>(readU32(buffer, offset));
}

inline void writeU32(std::span<uint8_t> buffer, size_t offset, uint32_t value) {
    if (offset + sizeof(value) <= buffer.size()) {
        memcpy(buffer.data() + offset, &value, sizeof(value));
    }
}

inline uint32_t readU32(const uint8_t* p) {
    uint32_t value = 0;
    memcpy(&value, p, sizeof(value));
    return value;
}

inline void writeU32(uint8_t* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

}  // namespace fingerprint
}  // namespace android
