/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Device.h"

#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <android-base/logging.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

namespace android {
namespace fingerprint {

namespace {
constexpr char kDevicePath[] = "/dev/fingerprint_dev";
// Touch panel switches, in driver preference order. fingerprint_hal is the
// kernel HBP interface: bit 0 arms the low-power path for screen-off and bit 1
// makes the normal touch path emit fingerprint down/up events for screen-on.
// fingerprint_active only arms the low-power path; fp_enable is the legacy
// oplus_touchscreen_v2 node.
constexpr char kFpHalPath[] = "/proc/touchpanel/fingerprint_hal";
constexpr const char* kTpPaths[] = {
        kFpHalPath,
        "/proc/touchpanel/fingerprint_active",
        "/proc/touchpanel/fp_enable",
};
constexpr size_t kTpValueSize = 2;
// The touch driver may publish the proc node a little after the HAL starts.
// Open is attempted once per operation window so the first authentication
// does not wait indefinitely for a driver that never comes up.
constexpr int kTpOpenAttempts = 20;
constexpr useconds_t kTpOpenRetryDelayUs = 5 * 1000;
// Timeout used only when the wake descriptor cannot be created; with it the
// reader sleeps until an event or a teardown signal arrives.
constexpr int kEventPollFallbackTimeoutMs = 100;

// One netlink message: the header the kernel prepends to the payload.
struct NetlinkMessage {
    struct nlmsghdr header;
    driver::FingerprintMessage message;
};
}  // namespace

Device::~Device() {
    stopEventLoop();
    close();
}

int Device::ioctlSimple(unsigned long request, void* arg) {
    if (mFd < 0) {
        LOG(ERROR) << "Device: no fd is opened";
        return -1;
    }
    int status = 0;
    void* payload = (arg != nullptr) ? arg : &status;
    if (ioctl(mFd, request, payload) != 0) {
        LOG(ERROR) << "Device: ioctl 0x" << std::hex << request << " failed: " << strerror(errno);
        return -1;
    }
    if (arg == nullptr && status != 0) {
        return -1;
    }
    return 0;
}

int Device::open() {
    {
        std::lock_guard<std::mutex> lock(mTpMutex);
        mTpStateKnown = false;
        mTpReportEnabled = false;
        mTpPath = nullptr;
    }
    mFd = ::open(kDevicePath, O_RDWR);
    if (mFd < 0) {
        LOG(ERROR) << "Device: cannot open " << kDevicePath << ": " << strerror(errno);
        return -1;
    }
    return 0;
}

int Device::close() {
    if (mFd >= 0) {
        stopWaitFingerprintEvent();
        disable();
        ::close(mFd);
        mFd = -1;
    }
    {
        std::lock_guard<std::mutex> lock(mTpMutex);
        if (mTpFd >= 0) {
            ::close(mTpFd);
            mTpFd = -1;
        }
        mTpPath = nullptr;
        mTpReportEnabled = false;
        mTpStateKnown = false;
    }
    return 0;
}

int Device::stopWaitFingerprintEvent() {
    uint32_t value = 0;
    return ioctlSimple(driver::kIocStopWaitFingerprint, &value);
}

int Device::disable() {
    uint32_t value = 0;
    int ret = ioctlSimple(driver::kIocDisableIrq, &value);
    if (ioctlSimple(driver::kIocDisable, &value) != 0) {
        ret = -1;
    }
    return ret;
}

int Device::reset() {
    return ioctlSimple(driver::kIocReset);
}

int Device::controlPower(driver::PowerState state) {
    return ioctlSimple(state == driver::PowerState::On ? driver::kIocEnablePower
                                                       : driver::kIocDisablePower);
}

int Device::setGpioResetValue(uint32_t value) {
    return ioctlSimple(value != 0 ? driver::kIocResetGpioCtlHigh : driver::kIocResetGpioCtlLow);
}

int Device::lhbmTemperature(int* value) {
    int32_t temperature = 0;
    if (ioctl(mFd, driver::kIocLhbmTemperature, &temperature) != 0) {
        return -1;
    }
    *value = temperature;
    return 0;
}

int Device::setIrqEnabled(bool enable) {
    // The driver takes zero and writes its state back into the argument.
    uint32_t value = 0;
    const int ret = ioctlSimple(enable ? driver::kIocEnableIrq : driver::kIocDisableIrq, &value);
    LOG(INFO) << "Device: irq " << (enable ? "enable" : "disable") << " -> " << value;
    return ret;
}

int Device::setIntr3Enabled(bool enable) {
    uint32_t value = 0;
    const int ret = ioctlSimple(enable ? driver::kIocEnableIntr3 : driver::kIocDisableIntr3, &value);
    LOG(INFO) << "Device: intr3 " << (enable ? "enable" : "disable") << " -> " << value;
    return ret;
}

int Device::holdWakeLock(bool hold) {
    uint32_t value = 0;
    return ioctlSimple(hold ? driver::kIocWakeLockHold : driver::kIocWakeLockRelease, &value);
}

int Device::cleanTouchStatus() {
    uint32_t value = 0;
    return ioctlSimple(driver::kIocCleanTouchStatus, &value);
}

int Device::openTpNodeLocked() {
    int error = ENOENT;
    for (int attempt = 0; attempt < kTpOpenAttempts; ++attempt) {
        bool retryable = true;
        for (const char* path : kTpPaths) {
            const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
            if (fd >= 0) {
                mTpFd = fd;
                mTpPath = path;
                LOG(INFO) << "Device: fingerprint control node " << path;
                return 0;
            }
            if (errno != ENOENT) {
                error = errno;
                retryable = false;
            }
        }
        if (!retryable || attempt + 1 == kTpOpenAttempts) {
            break;
        }
        usleep(kTpOpenRetryDelayUs);
    }
    mTpPath = nullptr;
    return error;
}

bool Device::isHalTpNode() const {
    return mTpPath != nullptr && strcmp(mTpPath, kFpHalPath) == 0;
}

int Device::writeTpModeLocked() {
    if (mTpFd < 0) {
        return -1;
    }

    int mode = mTpEnabled ? 1 : 0;
    if (mTpReportEnabled) {
        if (!isHalTpNode()) {
            LOG(WARNING) << "Device: " << mTpPath
                         << " cannot report screen-on fingerprint touches";
            return -1;
        }
        mode |= 2;
    }

    char value[4] = {};
    const int length = snprintf(value, sizeof(value), "%d", mode);
    if (length > 0 &&
        ::write(mTpFd, value, kTpValueSize) == static_cast<ssize_t>(kTpValueSize)) {
        LOG(INFO) << "Device: touch mode " << mode;
        return 0;
    }

    LOG(WARNING) << "Device: cannot set " << mTpPath << ": " << strerror(errno);
    ::close(mTpFd);
    mTpFd = -1;
    mTpPath = nullptr;
    return -1;
}

int Device::setTpEnabled(bool enable) {
    std::lock_guard<std::mutex> lock(mTpMutex);
    if (mTpStateKnown && mTpEnabled == enable && (enable || !mTpReportEnabled)) {
        return 0;
    }

    if (mTpFd < 0) {
        const int openError = openTpNodeLocked();
        if (mTpFd < 0) {
            LOG(WARNING) << "Device: cannot open any fingerprint control node: "
                         << strerror(openError);
            mTpStateKnown = false;
            return -1;
        }
    }

    // The panel is held while the mode is changed, as the vendor service does,
    // and any touch it latched on the way in is dropped.
    holdWakeLock(true);
    cleanTouchStatus();

    const bool previousEnabled = mTpEnabled;
    const bool previousReport = mTpReportEnabled;
    mTpEnabled = enable;
    if (!enable) {
        // Disarming the low-power path must not leave screen-on reporting on.
        mTpReportEnabled = false;
    }

    const int ret = writeTpModeLocked();

    holdWakeLock(false);

    if (ret == 0) {
        mTpStateKnown = true;
    } else {
        mTpEnabled = previousEnabled;
        mTpReportEnabled = previousReport;
        mTpStateKnown = false;
    }
    LOG(VERBOSE) << "Device: touch panel " << (enable ? "enable" : "disable") << " -> " << ret;
    return ret;
}

int Device::setTpReportEnabled(bool enable) {
    std::lock_guard<std::mutex> lock(mTpMutex);
    if (mTpStateKnown && mTpReportEnabled == enable) {
        return 0;
    }

    const bool previousEnabled = mTpEnabled;
    if (!mTpEnabled) {
        if (!enable) {
            mTpReportEnabled = false;
            return 0;
        }
        // Initial arming may have raced the driver's procfs registration.
        // Enable both bits now so this operation still gets touches.
        mTpEnabled = true;
    }

    if (mTpFd < 0) {
        const int openError = openTpNodeLocked();
        if (mTpFd < 0) {
            LOG(WARNING) << "Device: cannot open any fingerprint control node: "
                         << strerror(openError);
            mTpEnabled = previousEnabled;
            mTpStateKnown = false;
            return -1;
        }
    }

    if (!isHalTpNode() && enable) {
        // The initial arm may have picked a legacy node before the driver
        // registered fingerprint_hal. Retry once before giving up.
        ::close(mTpFd);
        mTpFd = -1;
        mTpPath = nullptr;
        const int openError = openTpNodeLocked();
        if (mTpFd < 0) {
            LOG(WARNING) << "Device: cannot open fingerprint_hal: " << strerror(openError);
            mTpEnabled = previousEnabled;
            return -1;
        }
        if (!isHalTpNode()) {
            LOG(WARNING) << "Device: screen-on fingerprint reporting is unavailable";
            mTpEnabled = previousEnabled;
            return -1;
        }
    }

    if (!isHalTpNode()) {
        // No screen-on report to disable on a legacy node.
        mTpReportEnabled = false;
        return 0;
    }

    holdWakeLock(true);
    cleanTouchStatus();

    const bool previousReport = mTpReportEnabled;
    mTpReportEnabled = enable;

    const int ret = writeTpModeLocked();

    holdWakeLock(false);

    if (ret == 0) {
        mTpStateKnown = true;
    } else {
        mTpEnabled = previousEnabled;
        mTpReportEnabled = previousReport;
        mTpStateKnown = false;
    }
    LOG(VERBOSE) << "Device: screen-on fingerprint report "
                 << (enable ? "enable" : "disable") << " -> " << ret;
    return ret;
}

int Device::startEventLoop(EventCallback callback) {
    if (mFd < 0) {
        return -1;
    }
    // A failed HAL init attempt can be retried with the reader already running.
    if (mRunning) {
        return 0;
    }
    // Ask the driver for its netlink transport, then find out which one it
    // actually took. If the request fails, the internal queue is still a valid
    // transport, so start the reader rather than leaving the HAL without
    // events.
    const int netlinkRet = ioctlSimple(driver::kIocNetlinkInit);
    int32_t transport = driver::kTransportInterrupt;
    (void)ioctl(mFd, driver::kIocReadNetlinkValue, &transport);
    if (netlinkRet != 0 && transport != driver::kTransportNetlink) {
        LOG(WARNING) << "Device: netlink init failed; using the internal queue";
    }
    if (transport != driver::kTransportNetlink) {
        transport = driver::kTransportInterrupt;
    }

    // Breaks the reader out of poll() at teardown; without it the reader would
    // have to wake on a timer to notice the run flag.
    mWakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (mWakeFd < 0) {
        LOG(WARNING) << "Device: cannot create the wake descriptor: " << strerror(errno);
    }

    mCallback = std::move(callback);
    mRunning = true;
    mEventThread = std::thread(&Device::eventThreadLoop, this, transport);
    LOG(INFO) << "Device: event loop started (transport " << transport << ")";
    return 0;
}

void Device::stopEventLoop() {
    if (!mRunning) {
        return;
    }
    mRunning = false;
    if (mWakeFd >= 0) {
        const uint64_t value = 1;
        (void)write(mWakeFd, &value, sizeof(value));
    }
    // Wakes a reader blocked in either transport so it can observe the flag.
    stopWaitFingerprintEvent();
    if (mEventThread.joinable()) {
        mEventThread.join();
    }
    if (mWakeFd >= 0) {
        ::close(mWakeFd);
        mWakeFd = -1;
    }
}

void Device::eventThreadLoop(int32_t transport) {
    if (transport == driver::kTransportNetlink) {
        runNetlinkLoop();
    } else {
        runInterruptLoop();
    }
    LOG(INFO) << "Device: event loop finished";
}

void Device::runNetlinkLoop() {
    const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, driver::kNetlinkProtocol);
    if (fd < 0) {
        LOG(WARNING) << "Device: event socket failed: " << strerror(errno);
        return;
    }

    struct sockaddr_nl local = {};
    local.nl_family = AF_NETLINK;
    local.nl_pid = getpid();
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) != 0) {
        LOG(WARNING) << "Device: event socket bind failed: " << strerror(errno);
        ::close(fd);
        return;
    }

    // The driver only unicasts once it has seen a message from this socket: its
    // receive callback records the sender's port id. Without this registration
    // it drops every event and recv() blocks forever.
    NetlinkMessage registration = {};
    registration.header.nlmsg_len = sizeof(registration);
    registration.header.nlmsg_pid = getpid();

    struct sockaddr_nl kernel = {};
    kernel.nl_family = AF_NETLINK;
    if (sendto(fd, &registration, sizeof(registration), 0,
               reinterpret_cast<struct sockaddr*>(&kernel), sizeof(kernel)) < 0) {
        LOG(WARNING) << "Device: event socket registration failed: " << strerror(errno);
        ::close(fd);
        return;
    }
    LOG(INFO) << "Device: netlink event reader registered";

    struct pollfd pfds[2] = {};
    pfds[0].fd = fd;
    pfds[0].events = POLLIN;
    nfds_t pollCount = 1;
    if (mWakeFd >= 0) {
        pfds[1].fd = mWakeFd;
        pfds[1].events = POLLIN;
        pollCount = 2;
    }
    // Block until there is work; the timeout only exists for the case where no
    // wake descriptor could be created.
    const int pollTimeout = (mWakeFd >= 0) ? -1 : kEventPollFallbackTimeoutMs;

    while (mRunning) {
        const int ready = poll(pfds, pollCount, pollTimeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (mRunning) {
                LOG(WARNING) << "Device: event poll failed: " << strerror(errno);
            }
            break;
        }
        if (ready == 0) {
            continue;
        }
        if (mWakeFd >= 0 && (pfds[1].revents & POLLIN) != 0) {
            break;
        }
        if (pfds[0].revents == 0) {
            continue;
        }

        NetlinkMessage received = {};
        const ssize_t length = recv(fd, &received, sizeof(received), 0);
        if (length < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (mRunning) {
                LOG(WARNING) << "Device: event read failed: " << strerror(errno);
            }
            break;
        }
        if (length < static_cast<ssize_t>(sizeof(received))) {
            continue;
        }
        dispatchEvent(received.message);
    }

    ::close(fd);
}

void Device::runInterruptLoop() {
    while (mRunning) {
        driver::FingerprintMessage message = {};
        const ssize_t length = read(mFd, &message, sizeof(message));
        if (length < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (mRunning) {
                LOG(WARNING) << "Device: event read failed: " << strerror(errno);
            }
            break;
        }
        if (length != static_cast<ssize_t>(sizeof(message))) {
            continue;
        }
        dispatchEvent(message);
    }
}

void Device::dispatchEvent(const driver::FingerprintMessage& message) {
    LOG(VERBOSE) << "Device: event module " << message.module << " code " << message.event;
    if (mCallback) {
        mCallback(message);
    }
}

}  // namespace fingerprint
}  // namespace android
