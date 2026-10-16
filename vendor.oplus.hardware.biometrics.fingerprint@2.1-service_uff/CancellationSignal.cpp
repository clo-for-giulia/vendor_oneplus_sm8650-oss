/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "CancellationSignal.h"

#include <utility>

namespace android {
namespace fingerprint {

CancellationSignal::CancellationSignal(std::function<void()> onCancel)
    : mOnCancel(std::move(onCancel)) {}

ndk::ScopedAStatus CancellationSignal::cancel() {
    if (!mCancelled.exchange(true) && mOnCancel) {
        mOnCancel();
    }
    return ndk::ScopedAStatus::ok();
}

}  // namespace fingerprint
}  // namespace android
