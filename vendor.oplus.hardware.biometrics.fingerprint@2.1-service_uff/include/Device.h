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

    // Points the touch panel at the fingerprint driver. Until this is set the
    // panel keeps delivering touches to the framework and the driver sees
    // nothing.
    int setTpEnabled(bool enable);
    // Clears the panel's pending touch flag.
    int cleanTouchStatus();
    // Reads the interrupt line state.
    int checkIrqValue(int* value);
    // Holds the touch path while the panel mode is changed.
    int holdWakeLock(bool hold);

    // Switches the driver to the event transport and starts the reader thread.
    int startEventLoop(EventCallback callback);
    void stopEventLoop();

  private:
    int ioctlSimple(unsigned long request, void* arg = nullptr);
    // Runs the reader for the transport the driver selected.
    void eventThreadLoop(int32_t transport);
    // Blocks on the netlink socket the driver unicasts events to.
    void runNetlinkLoop();
    // Blocks on read() while the driver queues events internally instead.
    void runInterruptLoop();
    // Logs one event and hands it to the callback.
    void dispatchEvent(const driver::FingerprintMessage& message);

    int mFd = -1;
    std::atomic<bool> mRunning = false;
    std::thread mEventThread;
    EventCallback mCallback;
};

}  // namespace fingerprint
}  // namespace android
