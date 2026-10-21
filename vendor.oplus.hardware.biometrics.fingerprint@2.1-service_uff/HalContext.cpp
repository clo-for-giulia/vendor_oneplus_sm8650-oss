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
            // The capture can block on TA commands and on UI_READY; keep it off
            // the reader so UI_READY, touch-up and screen events still flow.
            postTouchDown(PerfTrace::Source::Kernel);
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

void HalContext::startWorkThread() {
    if (mWorkThread.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mWorkMutex);
        mWorkRunning = true;
        mTouchPending = false;
        mWorkActive = false;
    }
    mWorkThread = std::thread(&HalContext::workThreadLoop, this);
}

void HalContext::stopWorkThread() {
    {
        std::lock_guard<std::mutex> lock(mWorkMutex);
        mWorkRunning = false;
    }
    mWorkCond.notify_all();
    if (mWorkThread.joinable()) {
        mWorkThread.join();
    }
    std::lock_guard<std::mutex> lock(mWorkMutex);
    mTouchPending = false;
    mWorkActive = false;
}

void HalContext::postTouchDown(PerfTrace::Source source) {
    {
        std::lock_guard<std::mutex> lock(mWorkMutex);
        // Drop a second down while one is queued or running; the framework is
        // not ready for it and the vendor drops these too.
        if (!mWorkRunning || mTouchPending || mWorkActive) {
            mPerf.notePost(source, false);
            return;
        }
        // The overlay event belongs to the touch that is starting. Drop a post
        // left over from screen-off, wake or the previous operation before the
        // worker starts waiting for this one.
        if (mCommon != nullptr) {
            mCommon->resetUiready();
        }
        mPerf.begin(source);
        mPerf.notePost(source, true);
        mTouchPending = true;
    }
    mWorkCond.notify_one();
}

void HalContext::workThreadLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mWorkMutex);
            mWorkCond.wait(lock, [this] { return !mWorkRunning || mTouchPending; });
            if (!mWorkRunning) {
                break;
            }
            mWorkActive = true;
        }
        mPerf.mark("worker");
        int result = 0;
        if (mCore != nullptr) {
            result = mCore->doTouchDownEvent();
        }
        mPerf.mark("core");
        mPerf.finish(result);
        {
            std::lock_guard<std::mutex> lock(mWorkMutex);
            mTouchPending = false;
            mWorkActive = false;
        }
        mWorkDoneCond.notify_all();
    }
}

void HalContext::waitForWorkIdle() {
    std::unique_lock<std::mutex> lock(mWorkMutex);
    // New session operations call this after setting the work mode: a touch
    // that was queued but has not started must not run for the next operation.
    // A touch already in flight is waited for.
    if (!mWorkActive) {
        mTouchPending = false;
    }
    mWorkDoneCond.wait(lock, [this] { return !mWorkActive; });
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
    if (mDevice->startEventLoop(
                [this](const driver::FingerprintMessage& message) {
                    onDeviceEvent(message);
                }) != 0) {
        LOG(ERROR) << "HalContext: failed to start the fingerprint event reader";
        return -1;
    }

    // Arm the HBP low-power path now, before the first screen-off. The normal
    // screen-on report bit is enabled only while an operation is active.
    if (mDevice->setTpEnabled(true) != 0) {
        LOG(WARNING) << "HalContext: cannot arm the touch panel low-power path";
    }

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

    // Started after the interrupts are armed and the core is initialized; a
    // touch that arrives in this last instant is dropped by postTouchDown()
    // rather than dispatched without a core.
    startWorkThread();

    mInitialized = true;
    LOG(INFO) << "HalContext: initialised (TA=" << taNameToString(mTaName) << ")";
    return 0;
}

int HalContext::deinit() {
    if (mDevice != nullptr) {
        mDevice->setIrqEnabled(false);
        mDevice->setIntr3Enabled(false);
        mDevice->setTpEnabled(false);
        mDevice->stopEventLoop();
    }

    // Abort whatever the worker is running and wake it if it is waiting for the
    // UI, then wait for it to leave the core before tearing the rest down.
    if (mCore != nullptr) {
        mCore->setWorkMode(WorkMode::Idle);
        mCore->cancel();
    }
    if (mCommon != nullptr) {
        mCommon->cancelUiready();
    }
    stopWorkThread();

    if (mDevice != nullptr) {
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
