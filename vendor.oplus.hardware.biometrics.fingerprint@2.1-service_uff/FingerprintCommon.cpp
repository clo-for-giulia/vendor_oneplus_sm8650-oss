/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "FingerprintCommon.h"

#include <chrono>

namespace android {
namespace fingerprint {

int FingerprintCommon::waitUiready(int timeoutMs) {
    std::unique_lock<std::mutex> lock(mMutex);
    if (mUireadyPosted) {
        mUireadyPosted = false;
        return 0;
    }
    if (!mCond.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                        [this] { return mUireadyPosted || mUireadyCancelled; })) {
        return -1;
    }
    if (mUireadyCancelled) {
        return -1;
    }
    mUireadyPosted = false;
    return 0;
}

void FingerprintCommon::postUiready() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mUireadyPosted = true;
        mUireadyCancelled = false;
    }
    mCond.notify_all();
}

void FingerprintCommon::cancelUiready() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mUireadyPosted = false;
        mUireadyCancelled = true;
    }
    mCond.notify_all();
}

}  // namespace fingerprint
}  // namespace android
