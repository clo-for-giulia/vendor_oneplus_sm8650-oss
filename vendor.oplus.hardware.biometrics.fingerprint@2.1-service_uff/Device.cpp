/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Device.h"

#include "TouchAidl.h"

#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <android-base/logging.h>
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
// Touch panel switch. Writing the mode here points the panel at the
// fingerprint driver instead of the framework.
constexpr char kTpPath[] = "/proc/touchpanel/fp_enable";
constexpr size_t kTpValueSize = 2;
// Touch service node the fingerprint switch lives on.
constexpr int32_t kTpNodeFlag = 31;
// How long the netlink reader waits between checks of the run flag.
constexpr int kEventPollTimeoutMs = 100;

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

int Device::setTpEnabled(bool enable) {
    // The panel is held while the mode is changed, as the vendor service does,
    // and any touch it latched on the way in is dropped.
    holdWakeLock(true);
    cleanTouchStatus();

    // The panel is switched by writing the touch driver's node; when that node
    // is not reachable from here, the touch service performs the same write.
    int ret = -1;
    const int fd = ::open(kTpPath, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        char value[4] = {};
        const int length = snprintf(value, sizeof(value), "%d", enable ? 1 : 0);
        if (length > 0 && ::write(fd, value, kTpValueSize) == static_cast<ssize_t>(kTpValueSize)) {
            ret = 0;
        } else {
            LOG(WARNING) << "Device: cannot set " << kTpPath << ": " << strerror(errno);
        }
        ::close(fd);
    } else {
        LOG(INFO) << "Device: " << kTpPath << " is not reachable (" << strerror(errno)
                  << "), routing through the touch service";
        ret = TouchAidl::instance().writeNode(kTpNodeFlag, enable ? 1 : 0);
    }

    holdWakeLock(false);

    LOG(INFO) << "Device: touch panel " << (enable ? "enable" : "disable") << " -> " << ret;
    return ret;
}

int Device::startEventLoop(EventCallback callback) {
    if (mFd < 0) {
        return -1;
    }
    // Ask the driver for its netlink transport, then find out which one it
    // actually took.
    if (ioctlSimple(driver::kIocNetlinkInit) != 0) {
        return -1;
    }
    // The driver reports which transport it took; default to the internal
    // queue when the query itself fails.
    int32_t transport = driver::kTransportInterrupt;
    (void)ioctl(mFd, driver::kIocReadNetlinkValue, &transport);

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
    // Wakes a reader blocked in either transport so it can observe the flag.
    stopWaitFingerprintEvent();
    if (mEventThread.joinable()) {
        mEventThread.join();
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

    while (mRunning) {
        struct pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int ready = poll(&pfd, 1, kEventPollTimeoutMs);
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
