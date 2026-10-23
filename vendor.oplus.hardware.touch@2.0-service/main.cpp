/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "OplusTouch"

#include <android-base/logging.h>
#include <android/binder_auto_utils.h>
#include <android/binder_interface_utils.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <string>

#include "OplusTouch.h"

int main() {
    android::base::InitLogging(nullptr, android::base::LogdLogger());

    if (!ABinderProcess_setThreadPoolMaxThreadCount(4)) {
        LOG(ERROR) << "cannot configure the binder thread pool";
        return 1;
    }

    auto service = ndk::SharedRefBase::make<
            aidl::vendor::oplus::hardware::touch::OplusTouch>();
    if (service == nullptr) {
        LOG(FATAL) << "failed to allocate OplusTouch service";
        return 1;
    }

    const std::string instance =
            std::string(aidl::vendor::oplus::hardware::touch::IOplusTouch::descriptor) +
            "/default";
    const binder_status_t status =
            AServiceManager_addService(service->asBinder().get(), instance.c_str());
    if (status != STATUS_OK) {
        LOG(FATAL) << "failed to register " << instance << ": "
                   << static_cast<int>(status);
        return 1;
    }

    LOG(INFO) << "registered " << instance;
    ABinderProcess_joinThreadPool();
    return 0;
}
