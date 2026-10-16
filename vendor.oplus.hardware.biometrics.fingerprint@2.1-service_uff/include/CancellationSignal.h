/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FingerprintNotify.h"

#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnSession.h>

#include <atomic>
#include <functional>
#include <memory>

namespace android {
namespace fingerprint {

// Cancellation token handed back to the framework. Reproduces the
// ICancellationSignal implementation used by the vendor HAL.
class CancellationSignal : public aidl::android::hardware::biometrics::common::BnCancellationSignal {
  public:
    explicit CancellationSignal(std::function<void()> onCancel);
    ndk::ScopedAStatus cancel() override;

  private:
    std::function<void()> mOnCancel;
    std::atomic<bool> mCancelled{false};
};

}  // namespace fingerprint
}  // namespace android
