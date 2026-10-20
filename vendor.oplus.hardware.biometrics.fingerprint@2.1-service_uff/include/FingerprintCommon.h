/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <condition_variable>
#include <mutex>

namespace android {
namespace fingerprint {

// UI-ready handshake shared by the core and the optical path.
class FingerprintCommon {
  public:
    FingerprintCommon() = default;

    // Blocks until the UI-ready event arrives or the timeout (ms) elapses.
    int waitUiready(int timeoutMs = 500);

    // Signals the UI-ready wait.
    void postUiready();

    // Wakes a pending waitUiready and makes it fail. Used when an operation is
    // cancelled or the HAL is shutting down, so a capture waiting for the UI
    // does not keep the worker busy.
    void cancelUiready();

  private:
    std::mutex mMutex;
    std::condition_variable mCond;
    bool mUireadyPosted = false;
    bool mUireadyCancelled = false;
};

}  // namespace fingerprint
}  // namespace android
