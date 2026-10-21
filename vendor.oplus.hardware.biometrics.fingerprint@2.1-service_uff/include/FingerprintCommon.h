/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace android {
namespace fingerprint {

// UI-ready handshake shared by the core and the optical path.
class FingerprintCommon {
  public:
    FingerprintCommon() = default;

    // Blocks until the UI-ready event arrives or the timeout (ms) elapses.
    // When postedAgeUs is non-null it receives how long the event had already
    // been posted before this call; zero when it arrives while waiting.
    int waitUiready(int timeoutMs = 500, int64_t* postedAgeUs = nullptr);

    // Signals the UI-ready wait.
    void postUiready();

    // Drops a UI-ready post left behind by the previous operation. A new touch
    // must wait for the overlay belonging to that touch, not consume an event
    // that arrived while the display was off or waking.
    void resetUiready();

    // Drops a posted UI-ready event without clearing a pending cancellation.
    // Used when the panel reports that the overlay/HBM has gone away.
    void clearUiready();

    // Clears a cancellation left by the previous operation without dropping a
    // UI_READY that may already have been posted for the new one.
    void clearUireadyCancellation();

    // Wakes a pending waitUiready and makes it fail. Used when an operation is
    // cancelled or the HAL is shutting down, so a capture waiting for the UI
    // does not keep the worker busy.
    void cancelUiready();

  private:
    std::mutex mMutex;
    std::condition_variable mCond;
    std::chrono::steady_clock::time_point mUireadyAt;
    bool mUireadyPosted = false;
    bool mUireadyCancelled = false;
};

}  // namespace fingerprint
}  // namespace android
