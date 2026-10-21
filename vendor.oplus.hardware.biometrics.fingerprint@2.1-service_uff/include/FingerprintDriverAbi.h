/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Userspace ABI of the OPLUS under-display fingerprint platform driver
 * (/dev/fingerprint_dev). Values match the in-kernel definitions in the device
 * kernel tree; the README documents the full ioctl and event tables.
 */

#include <cstdint>

namespace android {
namespace fingerprint {
namespace driver {

// ---------------------------------------------------------------------------
// ioctl command set (magic 'O')
// ---------------------------------------------------------------------------

constexpr unsigned long kIocMagic = 'O';

// Teardown companion to kIocEnablePower, issued after the IRQ is disarmed.
constexpr unsigned long kIocDisable = (kIocMagic << 8) | 1;
constexpr unsigned long kIocReset = (kIocMagic << 8) | 2;
// Touch interrupt arming. The driver expects 1 in the argument to enable and
// its own disable code otherwise.
constexpr unsigned long kIocEnableIrq = (kIocMagic << 8) | 3;
constexpr unsigned long kIocDisableIrq = (kIocMagic << 8) | 4;
constexpr unsigned long kIocEnablePower = (kIocMagic << 8) | 7;
constexpr unsigned long kIocDisablePower = (kIocMagic << 8) | 8;
// Holds and releases the touch path around a panel mode change.
constexpr unsigned long kIocWakeLockHold = (kIocMagic << 8) | 0x12;
constexpr unsigned long kIocWakeLockRelease = (kIocMagic << 8) | 0x13;
// Clears the panel's pending touch flag.
constexpr unsigned long kIocCleanTouchStatus = (kIocMagic << 8) | 0x14;
// Stops the driver waiting for a fingerprint event.
constexpr unsigned long kIocStopWaitFingerprint = (kIocMagic << 8) | 0x17;
constexpr unsigned long kIocResetGpioCtlLow = (kIocMagic << 8) | 25;
constexpr unsigned long kIocResetGpioCtlHigh = (kIocMagic << 8) | 26;
constexpr unsigned long kIocNetlinkInit = (kIocMagic << 8) | 29;
// Reports the transport the driver is delivering events on: 1 netlink,
// 2 the internal queue drained with read().
constexpr unsigned long kIocReadNetlinkValue = (kIocMagic << 8) | 30;
constexpr unsigned long kIocLhbmTemperature = (kIocMagic << 8) | 31;
// Second interrupt line, armed alongside the touch IRQ.
constexpr unsigned long kIocEnableIntr3 = (kIocMagic << 8) | 0x22;
constexpr unsigned long kIocDisableIntr3 = (kIocMagic << 8) | 0x23;

// ---------------------------------------------------------------------------
// Power state
// ---------------------------------------------------------------------------

enum class PowerState : int32_t {
    Off = 0,
    On = 1,
};

// ---------------------------------------------------------------------------
// Event transport
// ---------------------------------------------------------------------------

// Event codes the optical path consumes.
constexpr int kEventTpTouchdown = 4;
constexpr int kEventTpTouchup = 5;
constexpr int kEventUiReady = 6;
constexpr int kEventUiDisappear = 7;

// Transports the driver can deliver on, as reported by kIocReadNetlinkValue.
constexpr int32_t kTransportNetlink = 1;
constexpr int32_t kTransportInterrupt = 2;
// Protocol the driver registers its kernel socket under.
constexpr int kNetlinkProtocol = 25;

constexpr int kMaxMessageSize = 128;

// Payload the driver queues for every event (kernel `fingerprint_message_t`).
// Touch events carry their coordinates in outBuf.
struct FingerprintMessage {
    int32_t module;
    int32_t event;
    int32_t inSize;
    char inBuf[kMaxMessageSize];
    int32_t outSize;
    char outBuf[kMaxMessageSize];
};

}  // namespace driver
}  // namespace fingerprint
}  // namespace android
