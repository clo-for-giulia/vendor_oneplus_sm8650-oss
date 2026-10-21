/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FingerprintNotify.h"

#include <aidl/android/hardware/biometrics/common/ICancellationSignal.h>
#include <aidl/android/hardware/biometrics/common/OperationContext.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnSession.h>
#include <aidl/android/hardware/biometrics/fingerprint/ISessionCallback.h>
#include <aidl/android/hardware/biometrics/fingerprint/PointerContext.h>
#include <aidl/android/hardware/keymaster/HardwareAuthToken.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace android {
namespace fingerprint {

class HalContext;

// AIDL session. Implements android.hardware.biometrics.fingerprint.ISession
// (v3) on top of IFingerprintCore. Reproduces `BiometricsFpSession`.
class BiometricsFpSession
    : public aidl::android::hardware::biometrics::fingerprint::BnSession {
  public:
    using ISessionCallback =
            aidl::android::hardware::biometrics::fingerprint::ISessionCallback;
    using ICancellationSignal =
            aidl::android::hardware::biometrics::common::ICancellationSignal;

    BiometricsFpSession(HalContext* context, int32_t sensorId, int32_t userId,
                        std::shared_ptr<ISessionCallback> callback);
    ~BiometricsFpSession() override;

    int init();

    ndk::ScopedAStatus generateChallenge() override;
    ndk::ScopedAStatus revokeChallenge(int64_t challenge) override;
    ndk::ScopedAStatus enroll(
            const aidl::android::hardware::keymaster::HardwareAuthToken& hat,
            std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus authenticate(int64_t operationId,
                                    std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteraction(std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus enumerateEnrollments() override;
    ndk::ScopedAStatus removeEnrollments(const std::vector<int32_t>& enrollmentIds) override;
    ndk::ScopedAStatus getAuthenticatorId() override;
    ndk::ScopedAStatus invalidateAuthenticatorId() override;
    ndk::ScopedAStatus resetLockout(
            const aidl::android::hardware::keymaster::HardwareAuthToken& hat) override;
    ndk::ScopedAStatus close() override;
    ndk::ScopedAStatus onPointerDown(int32_t pointerId, int32_t x, int32_t y, float minor,
                                     float major) override;
    ndk::ScopedAStatus onPointerUp(int32_t pointerId) override;
    ndk::ScopedAStatus onUiReady() override;
    ndk::ScopedAStatus authenticateWithContext(
            int64_t operationId,
            const aidl::android::hardware::biometrics::common::OperationContext& context,
            std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus enrollWithContext(
            const aidl::android::hardware::keymaster::HardwareAuthToken& hat,
            const aidl::android::hardware::biometrics::common::OperationContext& context,
            std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteractionWithContext(
            const aidl::android::hardware::biometrics::common::OperationContext& context,
            std::shared_ptr<ICancellationSignal>* out) override;
    ndk::ScopedAStatus onPointerDownWithContext(
            const aidl::android::hardware::biometrics::fingerprint::PointerContext& context) override;
    ndk::ScopedAStatus onPointerUpWithContext(
            const aidl::android::hardware::biometrics::fingerprint::PointerContext& context) override;
    ndk::ScopedAStatus onContextChanged(
            const aidl::android::hardware::biometrics::common::OperationContext& context) override;
    ndk::ScopedAStatus onPointerCancelWithContext(
            const aidl::android::hardware::biometrics::fingerprint::PointerContext& context) override;
    ndk::ScopedAStatus setIgnoreDisplayTouches(bool shouldIgnore) override;

  private:
    std::shared_ptr<ICancellationSignal> makeCancellationSignal();
    ndk::ScopedAStatus authenticateInternal(
            int64_t operationId, bool displayOn,
            std::shared_ptr<ICancellationSignal>* out);
    void notify(const FingerprintMsg& message);
    void dispatchTouch(bool down);
    // Enables normal-touch fingerprint reporting for the operation. The HBP
    // low-power path is armed once when the HAL starts and stays armed, so this
    // only controls the screen-on report bit.
    void setTouchReport(bool enable);
    // Binds this session's user database in the TA. One TA is shared by every
    // user's session, so this runs when the session is created; the framework
    // serializes sessions, as the vendor's createSession does.
    int bindGroup();

    HalContext* mContext = nullptr;
    int32_t mSensorId = 0;
    int32_t mUserId = 0;
    std::shared_ptr<ISessionCallback> mCallback;
    std::mutex mMutex;
    int64_t mChallenge = 0;
    int64_t mAuthenticatorId = 0;
    bool mClosed = false;
    bool mIgnoreDisplayTouches = false;
    // Cleared when the framework cancels an operation. The worker may still be
    // unwinding in the TA; late result callbacks from that operation must not
    // land on the operation the acknowledgement unblocked.
    std::atomic<bool> mAcceptNotifications{false};
};

}  // namespace fingerprint
}  // namespace android
