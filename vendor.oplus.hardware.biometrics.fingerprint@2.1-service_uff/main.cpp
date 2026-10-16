/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "BiometricsFingerprintAidl.h"
#include "Utils.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <unistd.h>

#include <cstdlib>
#include <string>

using aidl::android::hardware::biometrics::fingerprint::IFingerprint;
using android::fingerprint::BiometricsFingerprintAidl;
using android::fingerprint::Utils;

namespace {
constexpr int32_t kRetryCountNormal = 20;
constexpr int32_t kRetryCountFactory = 10;
constexpr int32_t kRetryDelayUs = 500000;
}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    LOG(INFO) << "[FP_HAL][Service] enter to fp aidl service";

    const bool factoryMode = Utils::isFtmMode() || Utils::isMetaMode();
    const int32_t retryCount = factoryMode ? kRetryCountFactory : kRetryCountNormal;

    // The stock HAL publishes both of these unconditionally at startup; the
    // framework and the OPLUS tooling read them.
    for (const char* key : {"vendor.fingerprint.aidl.support",
                            "persist.vendor.fingerprint.optical.support"}) {
        if (!Utils::setProperty(key, "1")) {
            LOG(WARNING) << "[FP_HAL][Service] failed to set " << key;
        }
    }

    ABinderProcess_setThreadPoolMaxThreadCount(4);

    auto hal = ndk::SharedRefBase::make<BiometricsFingerprintAidl>();

    bool initialized = false;
    for (int32_t attempt = retryCount; attempt > 0; --attempt) {
        if (hal->init() == 0) {
            initialized = true;
            break;
        }
        LOG(ERROR) << "[FP_HAL][Service] fail to init UFF FP HAL, retry_times = " << attempt;
        usleep(kRetryDelayUs);
    }
    if (!initialized) {
        LOG(ERROR) << "[FP_HAL][Service] giving up on HAL init";
        return EXIT_FAILURE;
    }
    LOG(INFO) << "[FP_HAL][Service] success to init UFF FP HAL!";

    const std::string instance = std::string(IFingerprint::descriptor) + "/default";
    binder_status_t status =
            AServiceManager_addService(hal->asBinder().get(), instance.c_str());
    if (status != STATUS_OK) {
        LOG(ERROR) << "[FP_HAL][Service] add to service failed, descriptor="
                   << IFingerprint::descriptor << " status=" << status;
        return EXIT_FAILURE;
    }
    LOG(INFO) << "[FP_HAL][Service] add to service!";

    LOG(INFO) << "[FP_HAL][Service] fingerprint aidl service joinRpcThreadpool";
    ABinderProcess_joinThreadPool();
    LOG(INFO) << "[FP_HAL][Service] fingerprint hwbinder service exited";
    return EXIT_FAILURE;
}
