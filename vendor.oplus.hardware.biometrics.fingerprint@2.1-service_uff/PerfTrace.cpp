/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "PerfTrace.h"

#include <android-base/logging.h>
#include <android-base/properties.h>

namespace android {
namespace fingerprint {

namespace {
constexpr char kPerfProperty[] = "persist.vendor.fingerprint.perf";

const char* sourceName(PerfTrace::Source source) {
    switch (source) {
        case PerfTrace::Source::Kernel:
            return "kernel";
        case PerfTrace::Source::Pointer:
            return "pointer";
    }
    return "unknown";
}
}  // namespace

bool PerfTrace::enabled() {
    return ::android::base::GetBoolProperty(kPerfProperty, false);
}

uint64_t PerfTrace::elapsedUs(Clock::time_point start) {
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

void PerfTrace::notePost(Source source, bool accepted) {
    switch (source) {
        case Source::Kernel:
            mKernelPosts.fetch_add(1, std::memory_order_relaxed);
            if (accepted) {
                mKernelAccepted.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case Source::Pointer:
            mPointerPosts.fetch_add(1, std::memory_order_relaxed);
            if (accepted) {
                mPointerAccepted.fetch_add(1, std::memory_order_relaxed);
            }
            break;
    }
    if (!accepted) {
        mDropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void PerfTrace::begin(Source source) {
    if (!enabled()) {
        mActive.store(false, std::memory_order_relaxed);
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    mStages.clear();
    mTa.clear();
    mSource = source;
    mStart = Clock::now();
    mLast = mStart;
    mKernelPostsAtBegin = mKernelPosts.load(std::memory_order_relaxed);
    mPointerPostsAtBegin = mPointerPosts.load(std::memory_order_relaxed);
    mKernelAcceptedAtBegin = mKernelAccepted.load(std::memory_order_relaxed);
    mPointerAcceptedAtBegin = mPointerAccepted.load(std::memory_order_relaxed);
    mDroppedAtBegin = mDropped.load(std::memory_order_relaxed);
    mActive.store(true, std::memory_order_relaxed);
}

void PerfTrace::mark(const char* stage) {
    if (!mActive.load(std::memory_order_relaxed)) {
        return;
    }

    const Clock::time_point now = Clock::now();
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mActive.load(std::memory_order_relaxed)) {
        return;
    }
    const uint64_t durationUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - mLast).count());
    mStages.push_back({stage, durationUs});
    mLast = now;
}

void PerfTrace::recordTa(uint32_t command, uint64_t durationUs) {
    if (!mActive.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mActive.load(std::memory_order_relaxed)) {
        return;
    }
    mTa.push_back({command, durationUs});
}

void PerfTrace::recordSetup(const char* stage, uint64_t durationUs) {
    if (!enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& entry : mSetup) {
        if (entry.first == stage) {
            entry.second = durationUs;
            return;
        }
    }
    mSetup.emplace_back(stage, durationUs);
}

void PerfTrace::clearSetup() {
    std::lock_guard<std::mutex> lock(mMutex);
    mSetup.clear();
}

void PerfTrace::finish(int result) {
    if (!mActive.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mActive.load(std::memory_order_relaxed)) {
        return;
    }

    const uint64_t totalUs = elapsedUs(mStart);
    const uint64_t kernelPosts = mKernelPosts.load(std::memory_order_relaxed) -
                                 mKernelPostsAtBegin;
    const uint64_t pointerPosts = mPointerPosts.load(std::memory_order_relaxed) -
                                  mPointerPostsAtBegin;
    const uint64_t kernelAccepted = mKernelAccepted.load(std::memory_order_relaxed) -
                                    mKernelAcceptedAtBegin;
    const uint64_t pointerAccepted = mPointerAccepted.load(std::memory_order_relaxed) -
                                     mPointerAcceptedAtBegin;
    const uint64_t dropped = mDropped.load(std::memory_order_relaxed) - mDroppedAtBegin;

    std::string line = "Perf: src=";
    line += sourceName(mSource);
    line += " total_us=" + std::to_string(totalUs);
    line += " result=" + std::to_string(result);
    line += " posts=kernel=" + std::to_string(kernelPosts) + "/" +
            std::to_string(kernelAccepted);
    line += " pointer=" + std::to_string(pointerPosts) + "/" +
            std::to_string(pointerAccepted);
    line += " dropped=" + std::to_string(dropped);

    for (const auto& entry : mSetup) {
        line += " setup_" + entry.first + "=" + std::to_string(entry.second);
    }

    line += " stages=";
    if (mStages.empty()) {
        line += "none";
    } else {
        bool first = true;
        for (const auto& stage : mStages) {
            if (!first) {
                line += ",";
            }
            first = false;
            line += stage.name + "=" + std::to_string(stage.durationUs);
        }
    }

    line += " ta=";
    if (mTa.empty()) {
        line += "none";
    } else {
        bool first = true;
        for (const auto& entry : mTa) {
            if (!first) {
                line += ",";
            }
            first = false;
            line += std::to_string(entry.command) + ":" + std::to_string(entry.durationUs);
        }
    }

    LOG(INFO) << line;
    mActive.store(false, std::memory_order_relaxed);
}

}  // namespace fingerprint
}  // namespace android
