/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FingerprintDriverAbi.h"
#include "FpTypes.h"

#include <memory>

namespace android {
namespace fingerprint {

class Device;
class FpCa;
class FingerprintCommon;
class FingerprintConfig;
class FingerprintNotify;
class IFingerprintCore;

// Process-wide owner of the fingerprint HAL subsystems.
class HalContext {
  public:
    HalContext();
    ~HalContext();

    HalContext(const HalContext&) = delete;
    HalContext& operator=(const HalContext&) = delete;

    // Brings up the CA, device and core. Returns 0 on success.
    int init();
    int deinit();

    FpCa* ca() const { return mCa.get(); }
    Device* device() const { return mDevice.get(); }
    FingerprintCommon* common() const { return mCommon.get(); }
    FingerprintConfig* config() const { return mConfig.get(); }
    FingerprintNotify* notify() const { return mNotify.get(); }
    IFingerprintCore* core() const { return mCore.get(); }

    TaName selectedTa() const { return mTaName; }
    bool isInitialized() const { return mInitialized; }

  private:
    int selectChip();
    void onDeviceEvent(const driver::FingerprintMessage& message);

    std::unique_ptr<FpCa> mCa;
    std::unique_ptr<Device> mDevice;
    std::unique_ptr<FingerprintCommon> mCommon;
    std::unique_ptr<FingerprintConfig> mConfig;
    std::unique_ptr<FingerprintNotify> mNotify;
    std::unique_ptr<IFingerprintCore> mCore;

    TaName mTaName = TaName::UffGx;
    bool mInitialized = false;
};

}  // namespace fingerprint
}  // namespace android
