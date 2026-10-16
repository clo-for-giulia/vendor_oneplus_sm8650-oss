/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "HalContext.h"

#include "Device.h"
#include "FpCa.h"
#include "FingerprintCommon.h"
#include "FingerprintConfig.h"
#include "FingerprintNotify.h"
#include "IFingerprintCore.h"
#include "Utils.h"

#include <android-base/logging.h>

namespace android {
namespace fingerprint {

HalContext::HalContext() = default;

HalContext::~HalContext() {
    deinit();
}

int HalContext::selectChip() {
    static const TaName kCandidates[] = {
            TaName::UffGx,  TaName::UffJv,  TaName::UffSilead,
            TaName::UffEgis, TaName::UffFpc, TaName::UffQcom,
    };
    for (TaName candidate : kCandidates) {
        const std::string_view ta = taNameToString(candidate);
        if (ta.empty()) {
            continue;
        }
        for (const char* path : kTaFirmwarePaths) {
            if (Utils::isFileExist(std::string(path) + "/" + std::string(ta) + ".mdt")) {
                mTaName = candidate;
                LOG(INFO) << "HalContext: selected TA " << ta << " from " << path;
                return 0;
            }
        }
    }
    LOG(WARNING) << "HalContext: no TA firmware found, defaulting to uff_gx";
    mTaName = TaName::UffGx;
    return 0;
}

void HalContext::onDeviceEvent(const driver::FingerprintMessage& message) {
    switch (message.event) {
        case driver::kEventTpTouchdown:
            if (mCore != nullptr) {
                mCore->doTouchDownEvent();
            }
            break;
        case driver::kEventTpTouchup:
            if (mCore != nullptr) {
                mCore->doTouchUpEvent();
            }
            break;
        case driver::kEventUiReady:
            if (mCommon != nullptr) {
                mCommon->postUiready();
            }
            break;
        default:
            break;
    }
}

int HalContext::init() {
    if (mInitialized) {
        return 0;
    }

    mConfig = std::make_unique<FingerprintConfig>();
    mConfig->init();

    selectChip();

    mCommon = std::make_unique<FingerprintCommon>();

    mDevice = std::make_unique<Device>();
    if (mDevice->open() != 0) {
        LOG(ERROR) << "HalContext: failed to open fingerprint device";
        return -1;
    }
    mDevice->controlPower(driver::PowerState::On);
    mDevice->reset();

    mCa = std::unique_ptr<FpCa>(createCa("qsee"));
    if (mCa == nullptr || mCa->startTa(mTaName) != 0) {
        LOG(ERROR) << "HalContext: failed to start TA";
        return -1;
    }

    mConfig->taConfigInit(mCa.get());

    // The driver is switched to the netlink transport here; this is what
    // delivers touch and UI-ready events. Started unconditionally so a TA
    // config query failure cannot leave the HAL without input events.
    mDevice->startEventLoop(
            [this](const driver::FingerprintMessage& message) { onDeviceEvent(message); });

    mNotify = std::make_unique<FingerprintNotify>();
    mCore = std::unique_ptr<IFingerprintCore>(createFingerprintCore(this));
    if (mCore == nullptr) {
        LOG(ERROR) << "HalContext: cannot create core";
        return -1;
    }
    mCore->setNotify(mNotify.get());
    if (mCore->init() != 0) {
        LOG(ERROR) << "HalContext: core init failed";
        return -1;
    }

    // Arm the interrupt lines last. The driver reports no touch events until
    // both the IRQ and INTR3 lines are enabled.
    if (mDevice->setIrqEnabled(true) != 0 || mDevice->setIntr3Enabled(true) != 0) {
        LOG(ERROR) << "HalContext: failed to arm the touch interrupts";
        return -1;
    }

    mInitialized = true;
    LOG(INFO) << "HalContext: initialised (TA=" << taNameToString(mTaName) << ")";
    return 0;
}

int HalContext::deinit() {
    if (mDevice != nullptr) {
        mDevice->setIrqEnabled(false);
        mDevice->setIntr3Enabled(false);
        mDevice->stopEventLoop();
        mDevice->close();
    }
    if (mCa != nullptr) {
        mCa->closeTa();
    }
    mCore.reset();
    mNotify.reset();
    mDevice.reset();
    mCommon.reset();
    mCa.reset();
    mConfig.reset();
    mInitialized = false;
    return 0;
}

}  // namespace fingerprint
}  // namespace android
