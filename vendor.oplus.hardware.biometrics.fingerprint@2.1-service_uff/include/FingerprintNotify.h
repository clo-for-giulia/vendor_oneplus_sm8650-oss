/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FpTypes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <utility>

namespace android {
namespace fingerprint {

/*
 * Internal event produced by FingerprintNotify and consumed by the AIDL
 * session. It never crosses the binder boundary, so it is a plain aggregate
 * rather than a wire-exact layout.
 */
struct FingerprintMsg {
    static constexpr int32_t kTypeAcquired = 1;
    static constexpr int32_t kTypeEnroll = 3;
    static constexpr int32_t kTypeRemoved = 4;
    static constexpr int32_t kTypeAuthenticated = 5;
    static constexpr int32_t kTypeInteractionDetected = 15;
    static constexpr int32_t kTypeError = -1;

    // android.hardware.biometrics.fingerprint.Error values.
    static constexpr int32_t kErrorUnableToProcess = 2;
    static constexpr int32_t kErrorCanceled = 5;

    int32_t type = 0;
    uint32_t gid = 0;
    uint32_t fid = 0;
    int32_t acquired = 0;  // acquired-info / error code
    int32_t remaining = 0;
    std::array<uint8_t, kHwAuthTokenSize> token = {};
};

// Translates TA results into FingerprintMsg events and hands them to the
// registered callback.
class FingerprintNotify {
  public:
    using Callback = std::function<void(const FingerprintMsg&)>;

    FingerprintNotify() = default;

    void setNotify(Callback callback) { mCallback = std::move(callback); }

    void notifyEnrollResult(uint32_t fid, uint32_t gid, int32_t remaining);
    void notifyAuthResult(uint32_t fid, uint32_t gid,
                          const std::array<uint8_t, kHwAuthTokenSize>& token);
    // Reports a touch from the keyguard's interaction-detect mode.
    void notifyInteractionDetected();
    void notifyRemove(uint32_t gid, uint32_t fid, uint32_t remaining);

    // Reports a failure to the framework. Takes an
    // android.hardware.biometrics.fingerprint.Error value; without it a failed
    // capture leaves the client waiting until its watchdog fires.
    void notifyError(int32_t error);

    // Maps a raw capture result code to a fingerprint_acquired_info value and
    // emits the corresponding notification.
    void checkAcquiredInfo(int32_t code);

  private:
    void notifyAcquiredInfo(int32_t info);
    void emit(const FingerprintMsg& message);

    Callback mCallback;
};

}  // namespace fingerprint
}  // namespace android
