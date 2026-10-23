/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Source implementation of the public vendor.oplus.hardware.touch.IOplusTouch
 * V2 interface. This intentionally accesses /proc/touchpanel directly instead
 * of forwarding to the stock touchDaemon native Binder service.
 */

#define LOG_TAG "OplusTouch"

#include "OplusTouch.h"

#include <android-base/logging.h>
#include <android/binder_status.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#include "TouchNodeMap.h"

namespace aidl::vendor::oplus::hardware::touch {
namespace {

constexpr size_t kMaxNodeWriteSize = 0x19000;
constexpr size_t kMaximumReadSize = 0x19000;

// Nodes which the legacy driver exposes but the HBP driver does not.
constexpr int32_t kDoubleTapEnableNode = 1;
constexpr int32_t kGestureMaskNode = 21;

bool isSyntheticNode(int32_t deviceId, int32_t nodeFlag, const std::string& path) {
    return deviceId == 0 &&
           (nodeFlag == kDoubleTapEnableNode || nodeFlag == kGestureMaskNode) &&
           ::access(path.c_str(), F_OK) != 0;
}

std::string gestureMaskToHex(uint32_t mask) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%x", mask);
    return buffer;
}

bool parseGestureMask(const std::string& text, uint32_t* mask) {
    if (mask == nullptr || text.empty()) {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        value > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    *mask = static_cast<uint32_t>(value);
    return true;
}

int32_t readFile(const std::string& path, size_t requestedSize, std::string* result) {
    if (result == nullptr) {
        return -1;
    }
    result->clear();

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG(ERROR) << "open " << path << " for read failed: " << std::strerror(errno);
        return -1;
    }

    const size_t size = std::clamp<size_t>(requestedSize, 1, kMaximumReadSize);
    std::string buffer(size, '\0');
    const ssize_t readCount = ::read(fd, buffer.data(), buffer.size());
    const int savedErrno = errno;
    ::close(fd);

    if (readCount < 0) {
        LOG(ERROR) << "read " << path << " failed: " << std::strerror(savedErrno);
        return -1;
    }

    buffer.resize(static_cast<size_t>(readCount));
    const size_t nul = buffer.find('\0');
    if (nul != std::string::npos) {
        buffer.resize(nul);
    }

    *result = std::move(buffer);
    return static_cast<int32_t>(readCount);
}

int32_t writeFile(const std::string& path, const std::string& data) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG(ERROR) << "open " << path << " for write failed: " << std::strerror(errno);
        return -1;
    }

    size_t written = 0;
    while (written < data.size()) {
        const ssize_t count = ::write(fd, data.data() + written, data.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR) << "write " << path << " failed: " << std::strerror(errno);
            ::close(fd);
            return -1;
        }
        if (count == 0) {
            LOG(ERROR) << "write " << path << " returned zero";
            ::close(fd);
            return -1;
        }
        written += static_cast<size_t>(count);
    }

    // Some procfs handlers report EINVAL for fsync. The write itself already
    // reached the driver, so do not turn that into a failure.
    if (::fsync(fd) < 0 && errno != EINVAL) {
        LOG(WARNING) << "fsync " << path << " failed: " << std::strerror(errno);
    }
    ::close(fd);
    return static_cast<int32_t>(written);
}

}  // namespace

ndk::ScopedAStatus OplusTouch::initialize(OplusTouchStatus* _aidl_return) {
    LOG(INFO) << "oplus touch source service initializing";
    if (_aidl_return != nullptr) {
        *_aidl_return = OplusTouchStatus::OK;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus OplusTouch::isTouchNodeSupport(int32_t in_deviceId, int32_t in_nodeFlag,
                                                  int32_t* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    *_aidl_return = -1;
    auto path = getTouchNodePath(in_deviceId, in_nodeFlag, nullptr);
    if (!path.has_value()) {
        LOG(DEBUG) << "unsupported deviceId=" << in_deviceId << " nodeFlag=" << in_nodeFlag;
        return ndk::ScopedAStatus::ok();
    }

    if (::access(path->c_str(), F_OK) == 0 ||
        isSyntheticNode(in_deviceId, in_nodeFlag, *path)) {
        *_aidl_return = 1;
    } else {
        LOG(DEBUG) << "missing node " << *path;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus OplusTouch::touchReadNodeFile(int32_t in_deviceId, int32_t in_nodeFlag,
                                                 std::string* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    if (in_nodeFlag == kTouchNodeListFlag) {
        *_aidl_return = listTouchNodes(in_deviceId);
        return ndk::ScopedAStatus::ok();
    }

    auto path = getTouchNodePath(in_deviceId, in_nodeFlag, nullptr);
    if (!path.has_value()) {
        LOG(DEBUG) << "read: unsupported deviceId=" << in_deviceId
                   << " nodeFlag=" << in_nodeFlag;
        _aidl_return->clear();
        return ndk::ScopedAStatus::ok();
    }

    size_t readSize = 0;
    path = getTouchNodePath(in_deviceId, in_nodeFlag, &readSize);
    if (!path.has_value()) {
        _aidl_return->clear();
        return ndk::ScopedAStatus::ok();
    }

    if (isSyntheticNode(in_deviceId, in_nodeFlag, *path)) {
        if (in_nodeFlag == kGestureMaskNode) {
            *_aidl_return = gestureMaskToHex(mGestureMask.load());
        } else {
            *_aidl_return = mDoubleTapEnabled.load() ? "1" : "0";
        }
        LOG(DEBUG) << "read synthetic nodeFlag=" << in_nodeFlag
                   << " value=" << *_aidl_return;
        return ndk::ScopedAStatus::ok();
    }

    std::string value;
    if (readFile(*path, readSize, &value) < 0) {
        // The stock direct-mode implementation returns an empty string with an
        // OK status when the node cannot be read.
        _aidl_return->clear();
        return ndk::ScopedAStatus::ok();
    }

    LOG(DEBUG) << "read " << *path << " (" << value.size() << " bytes)";
    *_aidl_return = std::move(value);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus OplusTouch::touchWriteNodeFile(int32_t in_deviceId, int32_t in_nodeFlag,
                                                  const std::string& in_info,
                                                  int32_t* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    *_aidl_return = writeNodeFile(in_deviceId, in_nodeFlag, in_info);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus OplusTouch::touchWriteBtInfo(int32_t in_deviceId, int32_t in_nodeFlag,
                                                const std::string& in_info,
                                                int32_t* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    // The stock implementation forwards this operation to the daemon with a
    // separate BT-pen message type. This device has no BT pen consumer in the
    // YAAP tree, so keep the public method but do not pretend a procfs write is
    // equivalent.
    LOG(WARNING) << "touchWriteBtInfo is not supported by the source service"
                 << " deviceId=" << in_deviceId << " nodeFlag=" << in_nodeFlag
                 << " length=" << in_info.size();
    *_aidl_return = -1;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus OplusTouch::touchWriteNodeFileOneWay(int32_t in_deviceId,
                                                        int32_t in_nodeFlag,
                                                        const std::string& in_info) {
    const int32_t result = writeNodeFile(in_deviceId, in_nodeFlag, in_info);
    if (result < 0) {
        LOG(DEBUG) << "one-way write failed deviceId=" << in_deviceId
                   << " nodeFlag=" << in_nodeFlag;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus OplusTouch::touchNotifyClient(int32_t in_clientFlag,
                                                 const OplusTouchInfo& in_info,
                                                 OplusTouchStatus* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    std::vector<std::shared_ptr<IOplusTouchEventCallback>> callbacks;
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        callbacks = mCallbacks;
    }

    for (const auto& callback : callbacks) {
        if (callback == nullptr) {
            continue;
        }

        ndk::ScopedAStatus status =
                static_cast<uint32_t>(in_clientFlag) < 0x10000
                        ? callback->touchSendCommandOneWay(in_clientFlag, in_info)
                        : callback->touchSendCommand(in_clientFlag, in_info);
        if (!status.isOk()) {
            LOG(WARNING) << "touchNotifyClient callback failed for clientFlag="
                         << in_clientFlag << ": " << status.getDescription();
        }
    }

    *_aidl_return = OplusTouchStatus::OK;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus OplusTouch::registerEventCallback(
        const std::shared_ptr<IOplusTouchEventCallback>& in_callback,
        OplusTouchStatus* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    if (in_callback == nullptr) {
        LOG(WARNING) << "registerEventCallback called with null callback";
        *_aidl_return = OplusTouchStatus::OK;
        return ndk::ScopedAStatus::ok();
    }

    std::lock_guard<std::mutex> lock(mCallbackMutex);
    for (const auto& callback : mCallbacks) {
        if (callback.get() == in_callback.get()) {
            LOG(DEBUG) << "duplicate callback register";
            *_aidl_return = OplusTouchStatus::OK;
            return ndk::ScopedAStatus::ok();
        }
    }

    mCallbacks.push_back(in_callback);
    LOG(INFO) << "registered touch event callback, count=" << mCallbacks.size();
    *_aidl_return = OplusTouchStatus::OK;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus OplusTouch::unregisterEventCallback(
        const std::shared_ptr<IOplusTouchEventCallback>& in_callback,
        OplusTouchStatus* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }

    if (in_callback == nullptr) {
        LOG(WARNING) << "unregisterEventCallback called with null callback";
        *_aidl_return = OplusTouchStatus::OK;
        return ndk::ScopedAStatus::ok();
    }

    std::lock_guard<std::mutex> lock(mCallbackMutex);
    mCallbacks.erase(std::remove_if(mCallbacks.begin(), mCallbacks.end(),
                                   [&](const auto& callback) {
                                       return callback.get() == in_callback.get();
                                   }),
                     mCallbacks.end());
    LOG(INFO) << "unregistered touch event callback, count=" << mCallbacks.size();
    *_aidl_return = OplusTouchStatus::OK;
    return ndk::ScopedAStatus::ok();
}

int32_t OplusTouch::writeNodeFile(int32_t deviceId, int32_t nodeFlag,
                                  const std::string& info) {
    if (info.empty()) {
        LOG(WARNING) << "write: empty payload for deviceId=" << deviceId
                     << " nodeFlag=" << nodeFlag;
        return -1;
    }
    if (info.size() > kMaxNodeWriteSize) {
        LOG(ERROR) << "write: payload too large (" << info.size() << " bytes)";
        return -1;
    }

    auto path = getTouchNodePath(deviceId, nodeFlag, nullptr);
    if (!path.has_value()) {
        LOG(DEBUG) << "write: unsupported deviceId=" << deviceId
                   << " nodeFlag=" << nodeFlag;
        return -1;
    }

    if (isSyntheticNode(deviceId, nodeFlag, *path)) {
        if (nodeFlag == kGestureMaskNode) {
            uint32_t mask = 0;
            if (!parseGestureMask(info, &mask)) {
                LOG(WARNING) << "invalid synthetic gesture mask: " << info;
                return -1;
            }
            mGestureMask.store(mask);
            LOG(INFO) << "stored synthetic gesture mask " << mask;
        } else {
            mDoubleTapEnabled.store(info != "0");
        }
        return static_cast<int32_t>(info.size());
    }

    const int32_t result = writeFile(*path, info);
    if (result >= 0) {
        LOG(DEBUG) << "wrote " << result << " bytes to " << *path;
    }
    return result;
}

}  // namespace aidl::vendor::oplus::hardware::touch
