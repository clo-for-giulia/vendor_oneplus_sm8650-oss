/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "OpticalCore.h"

#include "Device.h"
#include "FingerprintCommon.h"
#include "FingerprintNotify.h"
#include "HalContext.h"
#include "Utils.h"

#include <android-base/logging.h>
#include <unistd.h>

#include <array>
#include <vector>

namespace android {
namespace fingerprint {

namespace {
constexpr int kAuthUireadyTimeoutMs = 1000;
constexpr int kEnrollUireadyTimeoutMs = 500;
constexpr int kMaxCaptureRetries = 3;
}  // namespace

OpticalCore::OpticalCore(HalContext* context) : IFingerprintCore(context) {}

int OpticalCore::init() {
    return IFingerprintCore::init();
}

void OpticalCore::delayAfterUiready() {
    // Short settle delay after the framework reports the UI ready, as the
    // vendor waits before the first capture.
    usleep(1000);
}

void OpticalCore::refreshTemperature() {
    // The read itself is the shell temperature the driver exposes (thermal
    // zones, not tied to the panel mode); a failed read means the TA gets the
    // driver's "unknown" sentinel, as the vendor sends it.
    if (mContext->device()->lhbmTemperature(&mTemperature) != 0) {
        mTemperature = 1000;
        return;
    }
    if (mTemperature < -29 || mTemperature > 99) {
        mTemperature = 1000;
    }
}

int OpticalCore::abortEnroll(int ret) {
    // What the vendor's failure path does: tell the framework, end the enroll
    // session in the TA and abort the operation there. The finish is the half
    // that closes the session — without it the aborted enrollment stays open —
    // and the abort has to happen while the transport still works; otherwise
    // every later touch is rejected.
    reportError(isCancelled() ? FingerprintMsg::kErrorCanceled
                              : FingerprintMsg::kErrorUnableToProcess);
    enrollFinish();
    cancel();
    return ret;
}

int OpticalCore::abortAuth(int ret) {
    // The vendor ends a failed authenticate with the TA-side finish, which is
    // what releases the operation there.
    reportError(isCancelled() ? FingerprintMsg::kErrorCanceled
                              : FingerprintMsg::kErrorUnableToProcess);
    authenticateFinish();
    return ret;
}

int OpticalCore::enrollDown() {
    if (mContext == nullptr || mContext->device() == nullptr) {
        return -1;
    }
    mContext->device()->setGpioResetValue(1);

    refreshTemperature();
    const int temperatureRet = setEnrollTemperature(mTemperature);
    if (temperatureRet != 0) {
        return abortEnroll(temperatureRet);
    }

    if (mContext->common() != nullptr) {
        mContext->common()->waitUiready(kEnrollUireadyTimeoutMs);
        // A cancel wakes the wait so this worker can abort its TA state before
        // the cancellation signal acknowledges the framework.
        if (isCancelled()) {
            return abortEnroll(-1);
        }
    }
    delayAfterUiready();

    CaptureResult capture = {};
    int ret = captureImg(WorkMode::Enroll, 0, &capture);
    if (ret != 0) {
        return abortEnroll(ret);
    }

    int32_t feature = 0;
    const int featureRet = getFeature(WorkMode::Enroll, 0, &feature);
    if (featureRet != 0) {
        return abortEnroll(featureRet);
    }

    int32_t acquired = 0;
    uint32_t fid = 0;
    int32_t remaining = 0;
    ret = enrollImage(&acquired, &fid, &remaining);
    if (ret != 0) {
        return abortEnroll(ret);
    }

    if (remaining <= 0) {
        // Last frame: persist the template first, the way the vendor does. A
        // failed save is fatal — telling the framework the enrollment finished
        // would leave it with a fingerprint the TA never stored.
        const int saveRet = enrollSaveTemplate();
        if (saveRet != 0) {
            LOG(ERROR) << "OpticalCore: cannot save the enroll template: " << saveRet;
            return abortEnroll(saveRet);
        }
    }

    if (mNotify != nullptr) {
        mNotify->checkAcquiredInfo(acquired);
        mNotify->notifyEnrollResult(fid, mGroupId, remaining);
    }

    if (remaining <= 0) {
        // The vendor refreshes the enrollment list once the framework has been
        // told the enrollment is done; the TA answers with what it stored.
        std::vector<uint32_t> fingerIds;
        const int enumerateRet = enumerate(&fingerIds);
        if (enumerateRet != 0) {
            LOG(ERROR) << "OpticalCore: enumeration after enroll failed: " << enumerateRet;
        } else {
            LOG(INFO) << "OpticalCore: enrollments after save: " << fingerIds.size();
        }
    }

    return 0;
}

int OpticalCore::authenticateStart() {
    refreshTemperature();
    int32_t result = 0;
    return IFingerprintCore::authenticateStart(&result);
}

int OpticalCore::authenticateDown() {
    if (mContext == nullptr || mContext->device() == nullptr) {
        return -1;
    }
    mContext->device()->setGpioResetValue(1);

    const int startRet = authenticateStart();
    if (startRet != 0) {
        return abortAuth(startRet);
    }

    if (mContext->common() != nullptr) {
        mContext->common()->waitUiready(kAuthUireadyTimeoutMs);
        if (isCancelled()) {
            return abortAuth(-1);
        }
    }
    delayAfterUiready();

    int ret = 0;
    for (int attempt = 0; attempt < kMaxCaptureRetries; ++attempt) {
        CaptureResult capture = {};
        ret = captureImg(WorkMode::Authenticate, static_cast<uint32_t>(attempt), &capture);
        if (ret != 0) {
            break;
        }
        if (capture.resultType == static_cast<int32_t>(TaResult::NeedRetry) ||
            capture.resultType == static_cast<int32_t>(TaResult::NeedRetry2)) {
            continue;
        }
        break;
    }
    if (ret != 0) {
        return abortAuth(ret);
    }

    int32_t feature = 0;
    const int featureRet = getFeature(WorkMode::Authenticate, 0, &feature);
    if (featureRet != 0) {
        return abortAuth(featureRet);
    }

    int32_t fingerId = 0;
    int32_t result = 0;
    std::array<uint8_t, kHwAuthTokenSize> token = {};
    ret = authenticateCompare(0, &fingerId, &result, &token);
    if (ret != 0) {
        reportError(isCancelled() ? FingerprintMsg::kErrorCanceled
                                  : FingerprintMsg::kErrorUnableToProcess);
    }
    authenticateFinish();

    if (mNotify != nullptr) {
        if (fingerId > 0) {
            mNotify->notifyAuthResult(static_cast<uint32_t>(fingerId), mGroupId, token);
        } else {
            mNotify->checkAcquiredInfo(result);
        }
    }

    return ret;
}

IFingerprintCore* createFingerprintCore(HalContext* context) {
    return new OpticalCore(context);
}

}  // namespace fingerprint
}  // namespace android
