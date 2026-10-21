/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace android {
namespace fingerprint {

// Opt-in, one-line tracing for a touch operation. Enabled with
// persist.vendor.fingerprint.perf=1. The disabled path is a few atomic loads
// and does not change operation flow.
class PerfTrace {
  public:
    enum class Source {
        Kernel,
        Pointer,
    };

    // Records a touch post even when tracing is disabled, so the per-operation
    // line can show whether both the kernel and AOSP pointer paths are active.
    void notePost(Source source, bool accepted);

    // Starts a trace for an accepted touch-down.
    void begin(Source source);

    // Records the time since the previous stage marker.
    void mark(const char* stage);

    // Records a TA command round trip while a trace is active.
    void recordTa(uint32_t command, uint64_t durationUs);

    // Records work done before the touch, such as AIDL setup or AuthenticateStart.
    void recordSetup(const char* stage, uint64_t durationUs);
    void clearSetup();

    // Emits the single trace line and ends the trace.
    void finish(int result);

    bool active() const { return mActive.load(std::memory_order_relaxed); }

  private:
    using Clock = std::chrono::steady_clock;

    struct Stage {
        std::string name;
        uint64_t durationUs = 0;
    };

    struct TaRecord {
        uint32_t command = 0;
        uint64_t durationUs = 0;
    };

    static bool enabled();
    static uint64_t elapsedUs(Clock::time_point start);

    std::atomic<bool> mActive{false};
    std::atomic<uint64_t> mKernelPosts{0};
    std::atomic<uint64_t> mPointerPosts{0};
    std::atomic<uint64_t> mKernelAccepted{0};
    std::atomic<uint64_t> mPointerAccepted{0};
    std::atomic<uint64_t> mDropped{0};

    std::mutex mMutex;
    Clock::time_point mStart;
    Clock::time_point mLast;
    uint64_t mResultUs = 0;
    Source mSource = Source::Kernel;
    uint64_t mKernelPostsAtBegin = 0;
    uint64_t mPointerPostsAtBegin = 0;
    uint64_t mKernelAcceptedAtBegin = 0;
    uint64_t mPointerAcceptedAtBegin = 0;
    uint64_t mDroppedAtBegin = 0;
    std::vector<Stage> mStages;
    std::vector<TaRecord> mTa;
    std::vector<std::pair<std::string, uint64_t>> mSetup;
};

}  // namespace fingerprint
}  // namespace android
