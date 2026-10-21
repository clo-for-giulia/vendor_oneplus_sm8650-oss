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
#include <chrono>
#include <vector>

namespace android {
namespace fingerprint {

namespace {
constexpr int kAuthUireadyTimeoutMs = 1000;
constexpr int kEnrollUireadyTimeoutMs = 500;
constexpr int kMaxCaptureRetries = 3;
// AOSP commonly starts authentication one to two seconds before the finger
// arrives. Keep the pre-armed command for the operation, but send it again from
// the touch path after ten seconds so the TA is not left with stale state.
constexpr int64_t kAuthPrearmMaxAgeNs = 10'000'000'000;

uint64_t elapsedUs(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
}
}  // namespace

OpticalCore::OpticalCore(HalContext* context) : IFingerprintCore(context) {}

int OpticalCore::init() {
    return IFingerprintCore::init();
}

void OpticalCore::delayAfterUiready(int64_t uireadyAgeUs) {
    // The vendor waits 15 ms after UI_READY before the first capture. Its
    // refresh-rate command is not sent by the AOSP framework, so the prebuilt
    // uses this default here as well. Starting 1 ms after UI_READY captures
    // before the overlay and panel dim layer have settled, which drops the
    // first screen-off press. A screen-on operation may consume an event that
    // was posted seconds earlier, so only wait for the unsettled remainder.
    constexpr int64_t kSettleUs = 15 * 1000;
    if (uireadyAgeUs >= kSettleUs) {
        return;
    }
    if (uireadyAgeUs < 0) {
        uireadyAgeUs = 0;
    }
    usleep(static_cast<useconds_t>(kSettleUs - uireadyAgeUs));
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
    mContext->perf().mark("enroll_enter");

    refreshTemperature();
    const int temperatureRet = setEnrollTemperature(mTemperature);
    if (temperatureRet != 0) {
        mContext->perf().mark("abort");
        return abortEnroll(temperatureRet);
    }
    mContext->perf().mark("enroll_down");

    int64_t uireadyAgeUs = 0;
    if (mContext->common() != nullptr) {
        const int uireadyRet = mContext->common()->waitUiready(kEnrollUireadyTimeoutMs,
                                                               &uireadyAgeUs);
        // A cancel wakes the wait so this worker can abort its TA state before
        // the cancellation signal acknowledges the framework. A timeout means
        // the overlay never became ready; capturing outside that window only
        // produces a bad frame and leaves the operation open.
        if (uireadyRet != 0 || isCancelled()) {
            mContext->perf().mark("abort");
            return abortEnroll(-1);
        }
    }
    mContext->perf().mark("ui_wait");
    delayAfterUiready(uireadyAgeUs);
    mContext->perf().mark("post_ui");

    CaptureResult capture = {};
    int ret = captureImg(WorkMode::Enroll, 0, &capture);
    if (ret != 0) {
        mContext->perf().mark("abort");
        return abortEnroll(ret);
    }
    mContext->perf().mark("capture");

    int32_t feature = 0;
    const int featureRet = getFeature(WorkMode::Enroll, 0, &feature);
    if (featureRet != 0) {
        mContext->perf().mark("abort");
        return abortEnroll(featureRet);
    }
    mContext->perf().mark("feature");

    int32_t acquired = 0;
    uint32_t fid = 0;
    int32_t remaining = 0;
    ret = enrollImage(&acquired, &fid, &remaining);
    if (ret != 0) {
        mContext->perf().mark("abort");
        return abortEnroll(ret);
    }
    mContext->perf().mark("enroll_image");

    if (remaining <= 0) {
        // Last frame: persist the template first, the way the vendor does. A
        // failed save is fatal — telling the framework the enrollment finished
        // would leave it with a fingerprint the TA never stored.
        const int saveRet = enrollSaveTemplate();
        if (saveRet != 0) {
            LOG(ERROR) << "OpticalCore: cannot save the enroll template: " << saveRet;
            mContext->perf().mark("abort");
            return abortEnroll(saveRet);
        }
        mContext->perf().mark("save");
    }

    if (mNotify != nullptr) {
        mNotify->checkAcquiredInfo(acquired);
        mNotify->notifyEnrollResult(fid, mGroupId, remaining);
    }
    mContext->perf().mark("notify");

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
    mContext->perf().mark("enumerate");

    return 0;
}

int OpticalCore::prepareAuthenticate() {
    if (mContext == nullptr || mContext->device() == nullptr) {
        return -1;
    }
    // AOSP starts the authentication operation before the finger arrives. The
    // TA start command only carries the temperature and panel brightness, so
    // send it here and keep it off the touch path. A failure is retried by
    // authenticateDown() when the touch actually arrives.
    mAuthPrepared = false;
    const auto prearmStart = std::chrono::steady_clock::now();
    refreshTemperature();
    int32_t result = 0;
    const int ret = IFingerprintCore::authenticateStart(&result);
    if (ret == 0) {
        mAuthPreparedAtNs = static_cast<int64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
        mAuthPrepared = true;
    }
    mContext->perf().recordSetup("prearm", elapsedUs(prearmStart));
    return ret;
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
    mContext->perf().mark("auth_enter");

    bool prepared = mAuthPrepared.exchange(false);
    if (prepared) {
        const int64_t now = static_cast<int64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
        prepared = now - mAuthPreparedAtNs.load() <= kAuthPrearmMaxAgeNs;
    }
    const int startRet = prepared ? 0 : authenticateStart();
    if (startRet != 0) {
        mContext->perf().mark("abort");
        return abortAuth(startRet);
    }
    mContext->perf().mark("auth_start");

    int64_t uireadyAgeUs = 0;
    if (mContext->common() != nullptr) {
        const int uireadyRet = mContext->common()->waitUiready(kAuthUireadyTimeoutMs,
                                                               &uireadyAgeUs);
        // The same overlay handshake gates authentication. A failed wait must
        // not fall through to a capture: the finger may already be gone and the
        // TA would be left in an operation that never produced a frame.
        if (uireadyRet != 0 || isCancelled()) {
            mContext->perf().mark("abort");
            return abortAuth(-1);
        }
    }
    mContext->perf().mark("ui_wait");
    delayAfterUiready(uireadyAgeUs);
    mContext->perf().mark("post_ui");

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
        mContext->perf().mark("abort");
        return abortAuth(ret);
    }
    mContext->perf().mark("capture");

    int32_t feature = 0;
    const int featureRet = getFeature(WorkMode::Authenticate, 0, &feature);
    if (featureRet != 0) {
        mContext->perf().mark("abort");
        return abortAuth(featureRet);
    }
    mContext->perf().mark("feature");

    int32_t fingerId = 0;
    int32_t result = 0;
    std::array<uint8_t, kHwAuthTokenSize> token = {};
    ret = authenticateCompare(0, &fingerId, &result, &token);
    if (ret != 0) {
        reportError(isCancelled() ? FingerprintMsg::kErrorCanceled
                                  : FingerprintMsg::kErrorUnableToProcess);
    }
    mContext->perf().mark("compare");

    // The token is complete after the compare. Publish it before the TA-side
    // finish so the framework can unlock while the cleanup still runs. The
    // worker stays busy until AuthenticateFinish returns, and the next
    // operation waits for it, so the TA still sees one operation at a time.
    if (mNotify != nullptr) {
        if (fingerId > 0) {
            mNotify->notifyAuthResult(static_cast<uint32_t>(fingerId), mGroupId, token);
        } else {
            mNotify->checkAcquiredInfo(result);
        }
    }
    mContext->perf().mark("result");

    authenticateFinish();
    mContext->perf().mark("finish");

    return ret;
}

IFingerprintCore* createFingerprintCore(HalContext* context) {
    return new OpticalCore(context);
}

}  // namespace fingerprint
}  // namespace android
