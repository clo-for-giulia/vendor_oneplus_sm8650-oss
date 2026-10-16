/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "HalContext.h"

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <aidl/android/hardware/biometrics/fingerprint/ISessionCallback.h>

#include <memory>
#include <vector>

namespace android {
namespace fingerprint {

// Top-level AIDL service. Implements
// android.hardware.biometrics.fingerprint.IFingerprint (v3). Reproduces
// `BiometricsFingerprintAidl`.
class BiometricsFingerprintAidl
    : public aidl::android::hardware::biometrics::fingerprint::BnFingerprint {
  public:
    using SensorProps = aidl::android::hardware::biometrics::fingerprint::SensorProps;
    using ISession = aidl::android::hardware::biometrics::fingerprint::ISession;
    using ISessionCallback =
            aidl::android::hardware::biometrics::fingerprint::ISessionCallback;

    BiometricsFingerprintAidl();
    ~BiometricsFingerprintAidl() override;

    // Brings up the HAL. Returns 0 on success.
    int init();

    // IFingerprint (v3).
    ndk::ScopedAStatus getSensorProps(std::vector<SensorProps>* out) override;
    ndk::ScopedAStatus createSession(int32_t sensorId, int32_t userId,
                                     const std::shared_ptr<ISessionCallback>& callback,
                                     std::shared_ptr<ISession>* out) override;
    binder_status_t dump(int fd, const char** args, uint32_t numArgs) override;

    HalContext* context() { return mContext.get(); }

  private:
    void fillComponentInfo(SensorProps* props);

    std::unique_ptr<HalContext> mContext;
    bool mInitialized = false;
};

}  // namespace fingerprint
}  // namespace android
