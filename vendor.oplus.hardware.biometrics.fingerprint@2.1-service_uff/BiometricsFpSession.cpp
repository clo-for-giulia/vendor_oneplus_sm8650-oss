/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "BiometricsFpSession.h"

#include "CancellationSignal.h"
#include "Device.h"
#include "FingerprintCommon.h"
#include "HalContext.h"
#include "IFingerprintCore.h"
#include "Utils.h"

#include <android-base/logging.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

using aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;
using aidl::android::hardware::biometrics::fingerprint::Error;
using aidl::android::hardware::keymaster::HardwareAuthToken;

namespace android {
namespace fingerprint {

namespace {
// The TA takes the auth token in its own serialized form rather than the AIDL
// one: a leading zero byte, the identifiers raw, the authenticator type and
// timestamp byte swapped and the MAC last. Reproduced from the vendor session,
// which builds the same buffer before sending its enroll command.
std::array<uint8_t, kTaAuthTokenSize> toTaAuthToken(const HardwareAuthToken& hat) {
    std::array<uint8_t, kTaAuthTokenSize> token = {};
    // token[0x00] stays zero and the MAC tail is zero-padded if short.
    memcpy(token.data() + 0x01, &hat.challenge, sizeof(hat.challenge));
    memcpy(token.data() + 0x09, &hat.userId, sizeof(hat.userId));
    memcpy(token.data() + 0x11, &hat.authenticatorId, sizeof(hat.authenticatorId));
    const uint32_t authenticatorType =
            __builtin_bswap32(static_cast<uint32_t>(hat.authenticatorType));
    memcpy(token.data() + 0x19, &authenticatorType, sizeof(authenticatorType));
    const uint64_t timestamp =
            __builtin_bswap64(static_cast<uint64_t>(hat.timestamp.milliSeconds));
    memcpy(token.data() + 0x1d, &timestamp, sizeof(timestamp));
    const size_t macLength = std::min<size_t>(hat.mac.size(), token.size() - 0x25);
    memcpy(token.data() + 0x25, hat.mac.data(), macLength);
    return token;
}

void mapAcquired(int32_t code, AcquiredInfo* info, int32_t* vendorCode) {
    switch (code) {
        case 1:
            *info = AcquiredInfo::GOOD;
            break;
        case 3:
            *info = AcquiredInfo::SENSOR_DIRTY;
            break;
        case 5:
            *info = AcquiredInfo::TOO_FAST;
            break;
        case 0x67:
        case 0x3e9:
        case 0x3ea:
        case 0x457:
        case 0x458:
        case 0x459:
        case 0x45c:
        default:
            *info = AcquiredInfo::VENDOR;
            *vendorCode = code;
            break;
    }
}
}  // namespace

BiometricsFpSession::BiometricsFpSession(HalContext* context, int32_t sensorId, int32_t userId,
                                         std::shared_ptr<ISessionCallback> callback)
    : mContext(context), mSensorId(sensorId), mUserId(userId), mCallback(std::move(callback)) {}

BiometricsFpSession::~BiometricsFpSession() {
    close();
}

int BiometricsFpSession::init() {
    if (mContext == nullptr || mContext->core() == nullptr) {
        return -1;
    }
    if (mContext->notify() != nullptr) {
        mContext->notify()->setNotify([this](const FingerprintMsg& message) { notify(message); });
    }
    // One TA serves every user's session, so bind this session's store here,
    // at the point the vendor's createSession binds it. A failure is not fatal:
    // the commands that touch data bind again before running.
    if (bindGroup() != 0) {
        LOG(WARNING) << "BiometricsFpSession: cannot bind the database for user " << mUserId;
    }
    return 0;
}

std::shared_ptr<BiometricsFpSession::ICancellationSignal>
BiometricsFpSession::makeCancellationSignal() {
    return ndk::SharedRefBase::make<CancellationSignal>([this]() {
        if (mContext == nullptr || mContext->core() == nullptr) {
            return;
        }
        // The framework cancels the operation's signal when it aborts the
        // operation — screen off, a higher-priority client, dismissal — and
        // never closes the session in between, so this is where the operation
        // ends. Release the work mode and the routing, abort the operation in
        // the TA, then acknowledge the cancellation: the framework's client
        // only finishes on that error, and without it the next operation (and
        // the overlay it shows) waits for the scheduler's watchdog.
        mContext->core()->setWorkMode(WorkMode::Idle);
        setTouchRouting(false);
        mContext->core()->cancel();
        if (mCallback != nullptr) {
            mCallback->onError(Error::CANCELED, 0);
        }
    });
}

void BiometricsFpSession::notify(const FingerprintMsg& message) {
    if (mCallback == nullptr) {
        return;
    }
    switch (message.type) {
        case FingerprintMsg::kTypeAcquired: {
            AcquiredInfo info = AcquiredInfo::VENDOR;
            int32_t vendorCode = 0;
            mapAcquired(message.acquired, &info, &vendorCode);
            mCallback->onAcquired(info, vendorCode);
            break;
        }
        case FingerprintMsg::kTypeEnroll:
            mCallback->onEnrollmentProgress(static_cast<int32_t>(message.fid), message.remaining);
            break;
        case FingerprintMsg::kTypeRemoved:
            mCallback->onEnrollmentsRemoved({static_cast<int32_t>(message.fid)});
            break;
        case FingerprintMsg::kTypeAuthenticated:
            if (message.fid != 0) {
                HardwareAuthToken token = {};
                mCallback->onAuthenticationSucceeded(static_cast<int32_t>(message.fid), token);
            } else {
                mCallback->onAuthenticationFailed();
            }
            break;
        case FingerprintMsg::kTypeError:
            // A capture interrupted by the framework's cancel reports CANCELED
            // here too, but the cancellation signal already acknowledged it.
            // Forwarding it would deliver the error twice, and a late copy
            // could land on the operation the acknowledgement unblocked.
            if (message.acquired != FingerprintMsg::kErrorCanceled) {
                mCallback->onError(static_cast<Error>(message.acquired), 0);
            }
            break;
        default:
            break;
    }
}

void BiometricsFpSession::dispatchTouch(bool down) {
    if (mIgnoreDisplayTouches || mContext == nullptr || mContext->core() == nullptr) {
        return;
    }
    if (down) {
        mContext->core()->doTouchDownEvent();
    } else {
        mContext->core()->doTouchUpEvent();
    }
}

void BiometricsFpSession::setTouchRouting(bool enable) {
    if (mContext != nullptr && mContext->device() != nullptr) {
        mContext->device()->setTpEnabled(enable);
    }
}

int BiometricsFpSession::bindGroup() {
    if (mContext == nullptr || mContext->core() == nullptr) {
        return -1;
    }
    char path[48];
    snprintf(path, sizeof(path), kUserDataPathFormat, mUserId);
    return mContext->core()->setActiveGroup(static_cast<uint32_t>(mUserId), path);
}

ndk::ScopedAStatus BiometricsFpSession::generateChallenge() {
    std::lock_guard<std::mutex> lock(mMutex);
    mChallenge = static_cast<int64_t>(Utils::getTimeUs());
    if (mCallback != nullptr) {
        mCallback->onChallengeGenerated(mChallenge);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::revokeChallenge(int64_t challenge) {
    std::lock_guard<std::mutex> lock(mMutex);
    mChallenge = 0;
    if (mCallback != nullptr) {
        mCallback->onChallengeRevoked(challenge);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::enroll(const HardwareAuthToken& hat,
                                               std::shared_ptr<ICancellationSignal>* out) {
    if (mContext == nullptr || mContext->core() == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    IFingerprintCore* core = mContext->core();
    bindGroup();
    core->preEnroll();
    core->enroll(toTaAuthToken(hat), static_cast<uint32_t>(mUserId));
    core->setWorkMode(WorkMode::Enroll);
    setTouchRouting(true);
    if (out != nullptr) {
        *out = makeCancellationSignal();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::authenticate(int64_t operationId,
                                                     std::shared_ptr<ICancellationSignal>* out) {
    if (mContext == nullptr || mContext->core() == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    IFingerprintCore* core = mContext->core();
    bindGroup();
    core->authenticate(static_cast<uint64_t>(operationId), static_cast<uint32_t>(mUserId));
    core->setWorkMode(WorkMode::Authenticate);
    setTouchRouting(true);
    if (out != nullptr) {
        *out = makeCancellationSignal();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::detectInteraction(
        std::shared_ptr<ICancellationSignal>* out) {
    if (mContext == nullptr || mContext->core() == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    // No TA command here: the group was bound when the session was created and
    // the vendor's detect path only arms the mode and the touch routing.
    mContext->core()->setWorkMode(WorkMode::Authenticate);
    setTouchRouting(true);
    if (out != nullptr) {
        *out = makeCancellationSignal();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::enumerateEnrollments() {
    if (mContext == nullptr || mContext->core() == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    bindGroup();
    std::vector<uint32_t> fingerIds;
    const int ret = mContext->core()->enumerate(&fingerIds);
    if (ret != 0) {
        // The framework does not catch service-specific errors from here; report
        // an empty list and let its records reconcile on the next boot.
        LOG(ERROR) << "BiometricsFpSession: enumeration failed: " << ret;
        fingerIds.clear();
    }
    if (mCallback != nullptr) {
        std::vector<int32_t> ids;
        ids.reserve(fingerIds.size());
        for (uint32_t id : fingerIds) {
            ids.push_back(static_cast<int32_t>(id));
        }
        mCallback->onEnrollmentsEnumerated(ids);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::removeEnrollments(
        const std::vector<int32_t>& enrollmentIds) {
    if (mContext == nullptr || mContext->core() == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    bindGroup();
    for (int32_t id : enrollmentIds) {
        // A failed removal must not surface as an exception: the framework does
        // not catch service-specific errors here and its system server would
        // restart. The core logs the TA's status.
        mContext->core()->remove(static_cast<uint32_t>(mUserId), static_cast<uint32_t>(id));
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::getAuthenticatorId() {
    if (mContext == nullptr || mContext->core() == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    bindGroup();
    uint64_t id = 0;
    const int ret = mContext->core()->getAuthenticatorId(&id);
    if (ret != 0) {
        LOG(ERROR) << "BiometricsFpSession: authenticator id query failed: " << ret;
    }
    if (mCallback != nullptr) {
        mCallback->onAuthenticatorIdRetrieved(static_cast<int64_t>(id));
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::invalidateAuthenticatorId() {
    if (mCallback != nullptr) {
        mCallback->onAuthenticatorIdInvalidated(0);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::resetLockout(const HardwareAuthToken& hat) {
    (void)hat;
    if (mCallback != nullptr) {
        mCallback->onLockoutCleared();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::close() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mClosed) {
        return ndk::ScopedAStatus::ok();
    }
    mClosed = true;
    setTouchRouting(false);
    if (mContext != nullptr && mContext->core() != nullptr) {
        // Drops the session's work mode, so touches arriving after it do nothing.
        mContext->core()->setWorkMode(WorkMode::Idle);
    }
    if (mContext != nullptr && mContext->notify() != nullptr) {
        mContext->notify()->setNotify(nullptr);
    }
    if (mCallback != nullptr) {
        mCallback->onSessionClosed();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::onPointerDown(int32_t pointerId, int32_t x, int32_t y,
                                                      float minor, float major) {
    (void)pointerId;
    (void)x;
    (void)y;
    (void)minor;
    (void)major;
    dispatchTouch(true);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::onPointerUp(int32_t pointerId) {
    (void)pointerId;
    dispatchTouch(false);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::onUiReady() {
    if (mContext != nullptr && mContext->common() != nullptr) {
        mContext->common()->postUiready();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::authenticateWithContext(
        int64_t operationId,
        const aidl::android::hardware::biometrics::common::OperationContext& context,
        std::shared_ptr<ICancellationSignal>* out) {
    (void)context;
    return authenticate(operationId, out);
}

ndk::ScopedAStatus BiometricsFpSession::enrollWithContext(
        const HardwareAuthToken& hat,
        const aidl::android::hardware::biometrics::common::OperationContext& context,
        std::shared_ptr<ICancellationSignal>* out) {
    (void)context;
    return enroll(hat, out);
}

ndk::ScopedAStatus BiometricsFpSession::detectInteractionWithContext(
        const aidl::android::hardware::biometrics::common::OperationContext& context,
        std::shared_ptr<ICancellationSignal>* out) {
    (void)context;
    return detectInteraction(out);
}

ndk::ScopedAStatus BiometricsFpSession::onPointerDownWithContext(
        const aidl::android::hardware::biometrics::fingerprint::PointerContext& context) {
    return onPointerDown(context.pointerId, static_cast<int32_t>(context.x),
                         static_cast<int32_t>(context.y), context.minor, context.major);
}

ndk::ScopedAStatus BiometricsFpSession::onPointerUpWithContext(
        const aidl::android::hardware::biometrics::fingerprint::PointerContext& context) {
    return onPointerUp(context.pointerId);
}

ndk::ScopedAStatus BiometricsFpSession::onContextChanged(
        const aidl::android::hardware::biometrics::common::OperationContext& context) {
    (void)context;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::onPointerCancelWithContext(
        const aidl::android::hardware::biometrics::fingerprint::PointerContext& context) {
    (void)context;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFpSession::setIgnoreDisplayTouches(bool shouldIgnore) {
    mIgnoreDisplayTouches = shouldIgnore;
    return ndk::ScopedAStatus::ok();
}

}  // namespace fingerprint
}  // namespace android
