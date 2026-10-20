/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FingerprintDriverAbi.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace android {
namespace fingerprint {

// Userspace wrapper around /dev/fingerprint_dev.
class Device {
  public:
    using EventCallback = std::function<void(const driver::FingerprintMessage&)>;

    Device() = default;
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] int open();
    int close();

    int reset();
    int controlPower(driver::PowerState state);
    // Teardown, run from close() before the fd goes away.
    int stopWaitFingerprintEvent();
    int disable();
    int setGpioResetValue(uint32_t value);
    int lhbmTemperature(int* value);

    // Arms or disarms the touch interrupt lines. The driver does not report any
    // event until the IRQ and INTR3 lines have both been enabled.
    int setIrqEnabled(bool enable);
    int setIntr3Enabled(bool enable);

    // Arms the low-power touch path. On the HBP kernel interface this is bit 0
    // of fingerprint_hal; it must be set before the screen turns off.
    int setTpEnabled(bool enable);
    // Enables or disables fingerprint down/up reporting from the normal
    // screen-on touch path. This is bit 1 of the HBP fingerprint_hal node; it
    // arms the low-power bit too if the startup arm raced the driver.
    int setTpReportEnabled(bool enable);
    // Clears the panel's pending touch flag.
    int cleanTouchStatus();
    // Holds the touch path while the panel mode is changed.
    int holdWakeLock(bool hold);

    // Switches the driver to the event transport and starts the reader thread.
    int startEventLoop(EventCallback callback);
    void stopEventLoop();

  private:
    int ioctlSimple(unsigned long request, void* arg = nullptr);
    // Opens the active touch driver's fingerprint control node. Requires
    // mTpMutex to be held.
    int openTpNodeLocked();
    // True when fingerprint_hal, rather than a legacy node, is open.
    bool isHalTpNode() const;
    // Writes the current touch mode bitmask. Requires mTpMutex to be held.
    int writeTpModeLocked();
    // Runs the reader for the transport the driver selected.
    void eventThreadLoop(int32_t transport);
    // Blocks on the netlink socket the driver unicasts events to.
    void runNetlinkLoop();
    // Blocks on read() while the driver queues events internally instead.
    void runInterruptLoop();
    // Logs one event and hands it to the callback.
    void dispatchEvent(const driver::FingerprintMessage& message);

    int mFd = -1;
    int mWakeFd = -1;
    // Kept open across operations. The touch driver publishes the control node
    // shortly after boot, so the first open retries briefly.
    int mTpFd = -1;
    const char* mTpPath = nullptr;
    std::mutex mTpMutex;
    // Last successfully applied touch-panel mode.
    bool mTpEnabled = false;
    bool mTpReportEnabled = false;
    bool mTpStateKnown = false;
    std::atomic<bool> mRunning = false;
    std::thread mEventThread;
    EventCallback mCallback;
};

}  // namespace fingerprint
}  // namespace android
