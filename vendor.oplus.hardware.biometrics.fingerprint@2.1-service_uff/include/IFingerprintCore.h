/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FpTypes.h"
#include "TaProtocol.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace android {
namespace fingerprint {

// Per-user directory the TA keeps the fingerprint data in. Sessions for
// different users share one TA, so every data-touching command re-binds the
// active group first; init binds user 0 for the queries made before any
// session exists.
constexpr char kUserDataPathFormat[] = "/data/vendor_de/%d/fpdata";

class HalContext;
class FingerprintNotify;

// Sensor-independent fingerprint engine. Reproduces `android::IFingerprintCore`
// (with `OpticalCore` supplying the UDFPS specifics). Every method maps 1:1 to
// a TA command whose id and field offsets were recovered from the vendor
// service (see TaProtocol.h).
class IFingerprintCore {
  public:
    struct CaptureResult {
        int32_t resultType = 0;
        int32_t resultValue = 0;
        bool ignoreTooFast = false;
        bool needTransmitTouchUp = false;
    };

    explicit IFingerprintCore(HalContext* context);
    virtual ~IFingerprintCore();

    IFingerprintCore(const IFingerprintCore&) = delete;
    IFingerprintCore& operator=(const IFingerprintCore&) = delete;

    void setNotify(FingerprintNotify* notify) { mNotify = notify; }

    // Brings the TA up: reads calibration state, loads calibration data if
    // needed and forwards the HMAC key.
    virtual int init();

    int setActiveGroup(uint32_t groupId, const char* path);
    int preEnroll();
    // Opens the enroll session in the TA, carrying the framework's auth token.
    // The algorithm stays out of enroll mode until this is sent, and rejects
    // every touch until it is.
    int enroll(const std::array<uint8_t, kHwAuthTokenSize>& token, uint32_t groupId);
    int setEnrollTemperature(int32_t temperature);
    int enrollImage(int32_t* acquiredInfo, uint32_t* fid, int32_t* remaining);
    int enrollSaveTemplate();
    int enrollFinish();
    int getAuthenticatorId(uint64_t* authenticatorId);
    int getFeature(WorkMode mode, uint32_t arg, int32_t* result);
    int captureImg(WorkMode mode, uint32_t retryIndex, CaptureResult* result);
    // Tells the TA that the finger is off the sensor while it is still working
    // on a press. Its answer (written to `retry` when non-null) says whether
    // the capture has to be run again.
    int transTouchUpStatusAndFinish(int32_t* retry);
    // Opens the authenticate session in the TA for `operationId`.
    int authenticate(uint64_t operationId, uint32_t groupId);
    int authenticateStart(int32_t* result);
    // On success, `token` receives the packed hw_auth_token_t the TA returns.
    int authenticateCompare(uint32_t authType, int32_t* fingerId, int32_t* result,
                            std::array<uint8_t, kHwAuthTokenSize>* token = nullptr);
    int authenticateStudy(uint32_t fingerId);
    int authenticateFinish();
    int enumerate(std::vector<uint32_t>* fingerIds);
    int remove(uint32_t groupId, uint32_t fingerId);
    int removeSingleFinger(uint32_t groupId, uint32_t fingerId, uint32_t remaining);
    // Applies the work mode, which is what the touch handler dispatches on.
    virtual int setWorkMode(WorkMode mode);
    WorkMode getWorkMode() const { return mWorkMode; }

    // High level UDFPS entry points invoked from the touch handler. Provided by
    // the sensor core (OpticalCore).
    virtual int enrollDown() = 0;
    virtual int authenticateDown() = 0;
    virtual int doTouchDownEvent();
    virtual int doTouchUpEvent();

    int cancel();

  protected:
    // Sends a fully built command buffer and returns the transport status
    // (0 == success).
    int sendTa(std::span<uint8_t> buffer);
    // Returns a zeroed command buffer of `size` with the module/cmd header.
    // The buffer is a per-thread scratch area; callers must not hold it across
    // another makeRequest() call.
    std::span<uint8_t> makeRequest(TaCmd cmd, size_t size,
                                   uint32_t module = kModuleFingerprint);
    // Streams a calibration blob to the TA in chunks.
    int writeCalibrationData(const std::vector<uint8_t>& data);
    // Reports a failed capture to the framework. Without it a failure leaves
    // the client waiting until its watchdog fires.
    void reportError(int32_t error);
    // True when cancel() has been called since the current operation started.
    bool isCancelled() const { return mCancelled; }

    HalContext* mContext = nullptr;
    FingerprintNotify* mNotify = nullptr;
    // Set by the session (operation start, cancel, close) and read by the
    // touch thread, so it has to be atomic.
    std::atomic<WorkMode> mWorkMode{WorkMode::Idle};
    uint64_t mOperationId = 0;
    uint32_t mGroupId = 0;
    int32_t mTemperature = 1000;
    std::atomic<bool> mCancelled = false;
    std::mutex mMutex;
};

// Creates the core matching the configured sensor (UDFPS / optical).
[[nodiscard]] IFingerprintCore* createFingerprintCore(HalContext* context);

}  // namespace fingerprint
}  // namespace android
