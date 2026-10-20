/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "IFingerprintCore.h"

#include <atomic>
#include <cstdint>

namespace android {
namespace fingerprint {

// UDFPS (optical under-display) specialisation. Reproduces
// `android::OpticalCore`: it drives the capture/enroll/authenticate sequence
// that runs when a display touch is reported.
class OpticalCore : public IFingerprintCore {
  public:
    explicit OpticalCore(HalContext* context);

    int init() override;
    int enrollDown() override;
    int prepareAuthenticate() override;
    int authenticateDown() override;

  private:
    int authenticateStart();
    void delayAfterUiready();
    // Reads the panel temperature for the TA, mapping anything out of range to
    // the driver's "unknown" sentinel.
    void refreshTemperature();
    // Ends a failed operation: reports it to the framework and aborts it in
    // the TA, so the algorithm is not left mid-operation.
    int abortEnroll(int ret);
    int abortAuth(int ret);

    // Set when AuthenticateStart has already been sent for the current
    // authentication. Consumed by the touch worker, which falls back to
    // sending it if the pre-arm did not run, failed or is stale.
    std::atomic<bool> mAuthPrepared{false};
    std::atomic<int64_t> mAuthPreparedAtNs{0};
};

}  // namespace fingerprint
}  // namespace android
