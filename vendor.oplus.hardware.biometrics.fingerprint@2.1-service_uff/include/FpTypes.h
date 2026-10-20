/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Shared value types for the OPLUS UFF fingerprint HAL. Numeric values were
 * recovered from the vendor service (see README.md).
 */

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace android {
namespace fingerprint {

// Packed hw_auth_token_t the TA returns: version, challenge, user id,
// authenticator id, authenticator type, timestamp, HMAC (68 bytes plus the
// leading version byte).
constexpr size_t kHwAuthTokenSize = 69;

// ---------------------------------------------------------------------------
// Trusted-application identities
// ---------------------------------------------------------------------------

// Index into the per-sensor TA table. The device tree in use (waffle / OnePlus
// 12) selects `uff_gx` (Goodix).
enum class TaName : int32_t {
    UffSpi = 0,     // "uff_spi"
    UffGx = 1,      // "uff_gx"
    UffJv = 2,      // "uff_jv"
    UffJvAlt = 3,   // "uff_jv"
    UffSilead = 4,  // "uff_silead"
    UffEgis = 5,    // "uff_egis"
    UffSilead2 = 6, // "uff_silead"
    UffGxAlt = 7,   // "uff_gx"
    UffFpc = 8,     // "uff_fpc"
    UffEgis2 = 9,   // "uff_egis"
    UffJv2 = 10,    // "uff_jv"
    UffJv3 = 11,    // "uff_jv"
    UffJv4 = 12,    // "uff_jv"
    UffQcom = 13,   // "uff_qcom"
    UffGx2 = 14,    // "uff_gx"
    UffJv5 = 15,    // "uff_jv"
    Max = 16,
};

// Concrete TA image name for a table index.
std::string_view taNameToString(TaName name);

// ---------------------------------------------------------------------------
// Fingerprint working modes
// ---------------------------------------------------------------------------

enum class WorkMode : int32_t {
    Idle = 0,
    // Sent to the TA in the capture and get-feature commands; the algorithm
    // only accepts these two values there.
    Enroll = 2,
    Authenticate = 3,
    // Keyguard interaction detection: no TA command. A touch-down is reported
    // as onInteractionDetected() and the framework starts the real
    // authentication; the value is the vendor's.
    Detect = 17,
};

}  // namespace fingerprint
}  // namespace android
