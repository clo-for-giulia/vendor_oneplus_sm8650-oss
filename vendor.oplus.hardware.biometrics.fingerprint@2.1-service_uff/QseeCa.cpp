/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "QseeCa.h"

#include <BufferAllocator/BufferAllocatorWrapper.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace android {
namespace fingerprint {

namespace {

// Size of the shared command/response header exchanged with the TA.
constexpr uint32_t kSharedHeaderSize = 0x40;
// Offset of the TA status word inside the shared header.
constexpr uint32_t kSharedStatusOffset = 0x10;
// Offset reported to the driver for the attached dma-buf.
constexpr uint32_t kCmdBufOffset = 4;

// keymaster64 TA request that yields the fingerprint HMAC key.
constexpr uint32_t kKeymasterCmd = 0x205;
constexpr uint32_t kKeymasterSub = 0x2;
constexpr uint32_t kKeymasterRespSize = 0x100;
constexpr char kKeymasterTa[] = "keymaster64";
constexpr char kKeymasterPath[] = "odm/vendor/firmware";

// QSEECom dma-buf heap, as registered by the Qualcomm securemsm kernel driver.
constexpr char kQseecomHeapName[] = "qcom,qseecom";
constexpr uint32_t kPageSize = 4096;

uint32_t alignPage(uint32_t size) {
    return (size + kPageSize - 1) & ~(kPageSize - 1);
}

BufferAllocator* bufferAllocator() {
    static BufferAllocator* allocator = CreateDmabufHeapBufferAllocator();
    return allocator;
}

int allocDmaBuf(size_t size, void** outMap) {
    BufferAllocator* allocator = bufferAllocator();
    if (allocator == nullptr) {
        LOG(ERROR) << "QseeCa: dmabuf allocator unavailable";
        return -1;
    }
    const int handleFd = DmabufHeapAlloc2(allocator, kQseecomHeapName, size, 0);
    if (handleFd < 0) {
        LOG(ERROR) << "QseeCa: DmabufHeapAlloc2(" << kQseecomHeapName << ") failed";
        return -1;
    }
    void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, handleFd, 0);
    if (map == MAP_FAILED) {
        ::close(handleFd);
        return -1;
    }
    *outMap = map;
    return handleFd;
}

void freeDmaBuf(int fd, void* map, size_t size) {
    if (map != nullptr && map != MAP_FAILED) {
        munmap(map, size);
    }
    if (fd >= 0) {
        ::close(fd);
    }
}

uint32_t readU32(const uint8_t* p) {
    uint32_t value = 0;
    memcpy(&value, p, sizeof(value));
    return value;
}

void writeU32(uint8_t* p, uint32_t value) {
    memcpy(p, &value, sizeof(value));
}

// The TA writes its own log into the payload buffer right past the command.
// The vendor service prints those lines after every successful send, dropping
// the debug and verbose levels; they carry the TA's diagnostics, so they are
// the first thing to read when a command is rejected. On the success path they
// are a logd write per line in the middle of a capture sequence, so they are
// opt-in there and always printed for a failed status.
bool taLogEnabled() {
    static const bool enabled =
            ::android::base::GetBoolProperty("persist.vendor.fingerprint.talog", false);
    return enabled;
}

void dumpTaLog(const void* payload, uint32_t cmdLength, uint32_t extraLength) {
    if (extraLength < 1) {
        return;
    }
    const auto* const base = static_cast<const uint8_t*>(payload);
    const char* line = reinterpret_cast<const char*>(base + cmdLength);
    const char* const end = line + extraLength;
    while (line < end && *line != '\0') {
        const char* const newline = static_cast<const char*>(memchr(line, '\n', end - line));
        if (newline == nullptr) {
            return;
        }
        const std::string_view text(line, newline - line);
        if (text.find("[D]") == std::string_view::npos &&
            text.find("[V]") == std::string_view::npos) {
            LOG(INFO) << "[talog] " << text;
        }
        line = newline + 1;
    }
}

}  // namespace

QseeCa::QseeCa() : FpCa("qsee") {}

QseeCa::~QseeCa() {
    closeTa();
}

int QseeCa::doStartTa(TaName name) {
    const std::string_view ta = taNameToString(name);
    if (ta.empty()) {
        LOG(ERROR) << "QseeCa: invalid TA index " << static_cast<int>(name);
        return -1;
    }
    const std::string appName(ta);
    for (const char* path : kTaFirmwarePaths) {
        mHandle = nullptr;
        if (QSEECom_start_app(&mHandle, path, appName.c_str(), kSharedHeaderSize) == 0) {
            return 0;
        }
    }
    LOG(ERROR) << "QseeCa: failed to load TA " << appName;
    return -1;
}

int QseeCa::doCloseTa() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mHandle != nullptr) {
        QSEECom_shutdown_app(&mHandle);
        mHandle = nullptr;
    }
    freePayloadBuffer();
    return 0;
}

int QseeCa::managePayloadBuffer(uint32_t size) {
    if (size <= mPayloadSize) {
        return 0;
    }
    void* map = nullptr;
    const int fd = allocDmaBuf(size, &map);
    if (fd < 0) {
        return -1;
    }
    freePayloadBuffer();
    mPayloadFd = fd;
    mPayloadMap = map;
    mPayloadSize = size;
    return 0;
}

void QseeCa::freePayloadBuffer() {
    freeDmaBuf(mPayloadFd, mPayloadMap, mPayloadSize);
    mPayloadFd = -1;
    mPayloadMap = nullptr;
    mPayloadSize = 0;
}

int QseeCa::doSendCommand(std::span<uint8_t> buffer) {
    if (buffer.empty() || mHandle == nullptr) {
        return -1;
    }

    // The command buffer carries the TA log buffer length at offset 0x1c; the
    // TA requires a dma-buf large enough for header + payload.
    const uint32_t extra = readU32(buffer.data() + 0x1c);
    const uint32_t length = static_cast<uint32_t>(buffer.size());
    const uint32_t total = extra + length;
    // The TA writes its response and log past the command itself, so the buffer
    // is rounded up to a page the way the vendor's allocator does. An
    // exact-size buffer is rejected by the TEE once the response outgrows it.
    const uint32_t allocated = alignPage(total);

    std::lock_guard<std::mutex> lock(mMutex);
    if (managePayloadBuffer(allocated) != 0) {
        LOG(ERROR) << "QseeCa: payload dma-buf allocation failed";
        return -1;
    }
    // Start from a clean log area: the TA only writes what it logs, and
    // dumpTaLog reads until the first NUL.
    memset(mPayloadMap, 0, mPayloadSize);
    memcpy(mPayloadMap, buffer.data(), length);

    auto* shared = static_cast<uint8_t*>(mHandle->ion_sbuffer);
    memset(shared, 0, kSharedHeaderSize);
    writeU32(shared, total);

    qsee::IonFdInfo ifd = {};
    ifd.data[0].fd = mPayloadFd;
    ifd.data[0].cmdBufOffset = kCmdBufOffset;

    const int ret = QSEECom_send_modified_cmd(mHandle, shared, kSharedHeaderSize, shared,
                                              kSharedHeaderSize, &ifd);

    uint32_t status = 0;
    if (ret == 0) {
        status = readU32(shared + kSharedStatusOffset);
        if (status != 0 || taLogEnabled()) {
            dumpTaLog(mPayloadMap, length, extra);
        }
    }

    memcpy(buffer.data(), mPayloadMap, length);

    if (ret != 0) {
        LOG(ERROR) << "QseeCa: send_modified_cmd failed: " << ret;
        return kTaSendFailed;
    }

    if (status != 0) {
        LOG(ERROR) << "QseeCa: TA status " << status;
        return static_cast<int>(status);
    }
    return 0;
}

int QseeCa::sendHmacKeyToFpta() {
    // Obtain the fingerprint HMAC key from the keymaster TA, then forward it to
    // the fingerprint TA (cmd 0x7e4).
    qsee::QSEECom_handle* keymasterHandle = nullptr;
    if (QSEECom_start_app(&keymasterHandle, kKeymasterPath, kKeymasterTa, kKeymasterRespSize) != 0) {
        LOG(ERROR) << "QseeCa: cannot start keymaster TA";
        return -1;
    }

    std::vector<uint8_t> keymasterResp(kKeymasterRespSize, 0);
    uint32_t request[2] = {kKeymasterCmd, kKeymasterSub};
    const int ret = QSEECom_send_cmd(keymasterHandle, request, sizeof(request), keymasterResp.data(),
                                     kKeymasterRespSize);
    QSEECom_shutdown_app(&keymasterHandle);
    if (ret != 0) {
        LOG(ERROR) << "QseeCa: keymaster HMAC request failed";
        return -1;
    }

    const uint32_t keyOffset = readU32(keymasterResp.data() + 4);
    const uint32_t keyLength = readU32(keymasterResp.data() + 8);
    LOG(INFO) << "QseeCa: keymaster HMAC key offset=" << keyOffset << " len=" << keyLength;

    // The keymaster response is sent in place as the TA command payload: the
    // 0x100-byte response lives at command offset 0x30, so the key (at
    // resp + key_offset) lands at command offset 0x30 + key_offset. The command
    // bypasses the normal framing, setting cmd_len explicitly.
    std::array<uint8_t, 0x130> buffer = {};
    writeU32(buffer.data(), kModuleFingerprint);
    writeU32(buffer.data() + 4, static_cast<uint32_t>(TaCmd::HmacKey));
    writeU32(buffer.data() + 0x18, static_cast<uint32_t>(buffer.size()));
    memcpy(buffer.data() + 0x30, keymasterResp.data(), kKeymasterRespSize);

    const int result = doSendCommand(buffer);
    if (result != 0) {
        LOG(ERROR) << "QseeCa: HMAC key transfer failed: " << result;
        return result;
    }
    LOG(INFO) << "QseeCa: HMAC key transferred";
    return 0;
}

}  // namespace fingerprint
}  // namespace android
