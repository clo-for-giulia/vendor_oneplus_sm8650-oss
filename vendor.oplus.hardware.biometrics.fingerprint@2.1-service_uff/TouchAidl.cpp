/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "TouchAidl.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>

#include <string>

namespace android {
namespace fingerprint {

namespace {
// Device id the touch service expects for the fingerprint node.
constexpr int32_t kTouchDevice = 0;
}  // namespace

TouchAidl& TouchAidl::instance() {
    static TouchAidl client;
    return client;
}

int TouchAidl::writeNode(int32_t nodeFlag, int32_t value) {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mTouch == nullptr) {
        // Non-blocking: this runs on the touch path, so waiting on the service
        // would stall the capture. A later call retries.
        const std::string instance = std::string(IOplusTouch::descriptor) + "/default";
        ndk::SpAIBinder binder(AServiceManager_getService(instance.c_str()));
        if (binder.get() == nullptr) {
            LOG(WARNING) << "TouchAidl: " << instance << " not available";
            return -1;
        }
        mTouch = IOplusTouch::fromBinder(binder);
        if (mTouch == nullptr) {
            LOG(WARNING) << "TouchAidl: cannot wrap " << instance;
            return -1;
        }
    }

    // The vendor only treats a negative result as "unsupported"; zero is a
    // valid answer.
    int32_t supported = 0;
    if (!mTouch->isTouchNodeSupport(kTouchDevice, nodeFlag, &supported).isOk() || supported < 0) {
        LOG(WARNING) << "TouchAidl: node " << nodeFlag << " unsupported";
        return -1;
    }

    int32_t result = 0;
    if (!mTouch->touchWriteNodeFile(kTouchDevice, nodeFlag, std::to_string(value), &result).isOk()) {
        LOG(WARNING) << "TouchAidl: write of " << value << " to node " << nodeFlag << " failed";
        return -1;
    }
    return 0;
}

}  // namespace fingerprint
}  // namespace android
