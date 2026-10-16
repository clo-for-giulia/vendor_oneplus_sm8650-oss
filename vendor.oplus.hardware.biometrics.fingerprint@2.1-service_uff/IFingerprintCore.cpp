/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "IFingerprintCore.h"

#include "Device.h"
#include "FpCa.h"
#include "FingerprintConfig.h"
#include "FingerprintNotify.h"
#include "HalContext.h"
#include "Utils.h"

#include <android-base/logging.h>

#include <ctime>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>

namespace android {
namespace fingerprint {

namespace {
constexpr char kCaliPath[] = "/mnt/vendor/persist/fingerprint/fingerprint_cali.bin";
constexpr uint32_t kCaliChunk = 0x100000;
// Panel node the vendor's factory test reads the maximum brightness from; the
// authenticate start passes it to the TA, which uses it to tune the capture.
constexpr char kMaxBrightnessPath[] = "/sys/kernel/oplus_display/max_brightness";

uint32_t readU32(std::span<const uint8_t> buffer, size_t offset) {
    uint32_t value = 0;
    if (offset + sizeof(value) <= buffer.size()) {
        memcpy(&value, buffer.data() + offset, sizeof(value));
    }
    return value;
}
int32_t readI32(std::span<const uint8_t> buffer, size_t offset) {
    return static_cast<int32_t>(readU32(buffer, offset));
}
void writeU32(std::span<uint8_t> buffer, size_t offset, uint32_t value) {
    if (offset + sizeof(value) <= buffer.size()) {
        memcpy(buffer.data() + offset, &value, sizeof(value));
    }
}

// Read once and kept: the vendor caches this in its factory-test state at init.
uint32_t panelMaxBrightness() {
    static const uint32_t value = [] {
        std::ifstream in(kMaxBrightnessPath);
        uint32_t brightness = 0;
        if (in.is_open()) {
            in >> brightness;
        }
        return brightness;
    }();
    return value;
}
}  // namespace

IFingerprintCore::IFingerprintCore(HalContext* context) : mContext(context) {}

IFingerprintCore::~IFingerprintCore() = default;

std::span<uint8_t> IFingerprintCore::makeRequest(TaCmd cmd, size_t size, uint32_t module) {
    static thread_local std::array<uint8_t, kMaxTaCommandSize> scratch;
    auto buffer = std::span<uint8_t>(scratch).first(size);
    std::fill(buffer.begin(), buffer.end(), 0);
    writeU32(buffer, 0x00, module);
    writeU32(buffer, 0x04, static_cast<uint32_t>(cmd));
    return buffer;
}

int IFingerprintCore::sendTa(std::span<uint8_t> buffer) {
    if (mContext == nullptr || mContext->ca() == nullptr) {
        return -1;
    }
    return mContext->ca()->sendCommand(buffer);
}

int IFingerprintCore::writeCalibrationData(const std::vector<uint8_t>& data) {
    const uint32_t total = static_cast<uint32_t>(data.size());
    for (uint32_t offset = 0; offset < total; offset += kCaliChunk) {
        const uint32_t chunk = std::min(kCaliChunk, total - offset);
        auto request = makeRequest(TaCmd::CalibrateWrite, chunk + 0x38, kModuleCalibrate);
        writeU32(request, 0x20, total);
        writeU32(request, 0x24, offset);
        writeU32(request, 0x30, chunk);
        memcpy(request.data() + 0x34, data.data() + offset, chunk);
        const int ret = sendTa(request);
        if (ret != 0) {
            LOG(ERROR) << "IFingerprintCore: calibration transfer failed: " << ret;
            return ret;
        }
    }
    return 0;
}

int IFingerprintCore::init() {
    std::lock_guard<std::mutex> lock(mMutex);

    auto request = makeRequest(TaCmd::Init, 0x1c0);
    const int ret = sendTa(request);
    if (ret != 0) {
        LOG(ERROR) << "IFingerprintCore: init command failed: " << ret;
        return ret;
    }

    const uint32_t caliState = readU32(request, 0xdc);
    LOG(INFO) << "IFingerprintCore: calibration state " << caliState;
    if (caliState == 0 && Utils::isFileExist(kCaliPath)) {
        std::ifstream in(kCaliPath, std::ios::binary | std::ios::ate);
        if (in.is_open()) {
            const auto size = static_cast<size_t>(in.tellg());
            std::vector<uint8_t> data(size);
            in.seekg(0);
            in.read(reinterpret_cast<char*>(data.data()), size);
            writeCalibrationData(data);
        }
    }

    FingerprintConfig* config = mContext->config();
    if (config != nullptr && config->hmacKeyGate()) {
        if (mContext->ca()->sendHmacKeyToFpta() != 0) {
            LOG(WARNING) << "IFingerprintCore: HMAC key transfer failed";
        }
    }

    // Bind the user database so the queries the framework makes at boot, before
    // any session exists — the authenticator id and the enrollment list — are
    // answered from the real state instead of an empty TA. The framework
    // reconciles its own records against the enrollment list, and removes
    // templates it does not know, which is how enrollments left behind by a
    // different ROM get cleaned up.
    char path[48];
    snprintf(path, sizeof(path), kUserDataPathFormat, 0);
    setActiveGroup(0, path);
    return 0;
}

int IFingerprintCore::setActiveGroup(uint32_t groupId, const char* path) {
    auto request = makeRequest(TaCmd::SetActiveGroup, 0x100);
    writeU32(request, 0x30, groupId);
    if (path != nullptr && strlen(path) <= 0x7f) {
        memcpy(request.data() + 0x34, path, strlen(path));
    }
    mGroupId = groupId;
    return sendTa(request);
}

int IFingerprintCore::preEnroll() {
    auto request = makeRequest(TaCmd::PreEnroll, 0x80);
    return sendTa(request);
}

int IFingerprintCore::enroll(const std::array<uint8_t, kTaAuthTokenSize>& token, uint32_t groupId) {
    auto request = makeRequest(TaCmd::Enroll, 0xc8);
    writeU32(request, 0x30, groupId);
    memcpy(request.data() + 0x35, token.data(), token.size());
    return sendTa(request);
}

int IFingerprintCore::setEnrollTemperature(int32_t temperature) {
    auto request = makeRequest(TaCmd::EnrollTemperature, 0x90);
    writeU32(request, 0x44, static_cast<uint32_t>(temperature));
    return sendTa(request);
}

int IFingerprintCore::enrollImage(int32_t* acquiredInfo, uint32_t* fid, int32_t* remaining) {
    auto request = makeRequest(TaCmd::EnrollImage, 0xb0);
    const int ret = sendTa(request);
    if (acquiredInfo != nullptr) {
        *acquiredInfo = readI32(request, 0x54);
    }
    if (fid != nullptr) {
        // The TA stores the enrollment id at +0x84 (the vendor's session takes
        // the high word of its 64-bit result id, which is this same value).
        *fid = readU32(request, 0x84);
    }
    if (remaining != nullptr) {
        *remaining = readI32(request, 0x8c);
    }
    return ret;
}

int IFingerprintCore::enrollSaveTemplate() {
    auto request = makeRequest(TaCmd::EnrollSaveTemplate, 0x80);
    return sendTa(request);
}

int IFingerprintCore::enrollFinish() {
    auto request = makeRequest(TaCmd::EnrollFinish, 0x30);
    return sendTa(request);
}

int IFingerprintCore::getAuthenticatorId(uint64_t* authenticatorId) {
    auto request = makeRequest(TaCmd::GetAuthenticatorId, 0x80);
    const int ret = sendTa(request);
    if (ret == 0 && authenticatorId != nullptr) {
        uint64_t id = 0;
        memcpy(&id, request.data() + 0x78, sizeof(id));
        *authenticatorId = id;
    }
    return ret;
}

int IFingerprintCore::getFeature(WorkMode mode, uint32_t arg, int32_t* result) {
    auto request = makeRequest(TaCmd::GetFeature, 0xa0);
    writeU32(request, 0x30, arg);
    writeU32(request, 0x34, static_cast<uint32_t>(mode));
    const int ret = sendTa(request);
    const int32_t value = readI32(request, 0x58);
    if (result != nullptr) {
        *result = value;
    }
    return ret;
}

int IFingerprintCore::captureImg(WorkMode mode, uint32_t retryIndex, CaptureResult* result) {
    auto request = makeRequest(TaCmd::Capture, 0xb8);
    writeU32(request, 0x30, retryIndex);
    writeU32(request, 0x34, static_cast<uint32_t>(mode));

    time_t now = time(nullptr);
    struct tm tm = {};
    localtime_r(&now, &tm);
    const uint32_t date = static_cast<uint32_t>((tm.tm_year << 16) | (tm.tm_mon << 8) | tm.tm_mday);
    writeU32(request, 0x4c, date);

    int ret = sendTa(request);
    if (ret != 0) {
        return ret;
    }

    // A tap that ends while the TA is capturing makes it ask for the finger-lift
    // status; it then has to see the capture again before it settles on a
    // result. Run the exchange the way the vendor does: transmit, and only run
    // the capture once more if the TA asked for it.
    if (request[0x95] != 0) {
        int32_t askedToRetry = 0;
        if (transTouchUpStatusAndFinish(&askedToRetry) == 0 && askedToRetry != 0) {
            memset(request.data() + 0x6c, 0, 0x40);
            ret = sendTa(request);
            if (ret != 0) {
                return ret;
            }
            if (request[0x95] != 0) {
                transTouchUpStatusAndFinish(nullptr);
            }
        }
    }

    if (result != nullptr) {
        result->resultType = readI32(request, 0x6c);
        result->resultValue = readI32(request, 0x70);
        result->ignoreTooFast = request[0x94] != 0;
        result->needTransmitTouchUp = request[0x95] != 0;
    }
    return 0;
}

int IFingerprintCore::transTouchUpStatusAndFinish(int32_t* retry) {
    auto request = makeRequest(TaCmd::TouchUpStatus, 0x188);
    // +0x6c tells the TA whether a touch message is still queued for this press.
    // Ours are handled inline, so there never is one; the field stays zero.
    const int ret = sendTa(request);
    if (retry != nullptr) {
        *retry = request[0x10d];
    }
    return ret;
}

int IFingerprintCore::authenticate(uint64_t operationId, uint32_t groupId) {
    auto request = makeRequest(TaCmd::Authenticate, 0x88);
    // Offsets recovered from the disassembly: +0x30 group, +0x38 operation id,
    // +0x40 scenario flag (always zero on this build).
    writeU32(request, 0x30, groupId);
    memcpy(request.data() + 0x38, &operationId, sizeof(operationId));
    return sendTa(request);
}

int IFingerprintCore::authenticateStart(int32_t* result) {
    auto request = makeRequest(TaCmd::AuthenticateStart, 0xa0);
    // Offsets recovered from the disassembly: +0x44 flag, +0x48 temperature,
    // +0x58 backlight brightness.
    writeU32(request, 0x44, 1);
    writeU32(request, 0x48, static_cast<uint32_t>(mTemperature));
    writeU32(request, 0x58, panelMaxBrightness());
    const int ret = sendTa(request);
    if (result != nullptr) {
        *result = ret;
    }
    return ret;
}

int IFingerprintCore::authenticateCompare(uint32_t authType, int32_t* fingerId, int32_t* result) {
    auto request = makeRequest(TaCmd::AuthCompare, 0xf0);
    writeU32(request, 0x30, authType);
    const int ret = sendTa(request);
    if (fingerId != nullptr) {
        *fingerId = static_cast<int32_t>(readU32(request, 0x80));
    }
    if (result != nullptr) {
        *result = readI32(request, 0x54);
    }
    return ret;
}

int IFingerprintCore::authenticateStudy(uint32_t fingerId) {
    auto request = makeRequest(TaCmd::AuthStudy, 0x80);
    writeU32(request, 0x50, fingerId);
    return sendTa(request);
}

int IFingerprintCore::authenticateFinish() {
    auto request = makeRequest(TaCmd::AuthFinish, 0x78);
    return sendTa(request);
}

int IFingerprintCore::enumerate(std::vector<uint32_t>* fingerIds) {
    auto request = makeRequest(TaCmd::Enumerate, 0x98);
    const int ret = sendTa(request);
    if (ret != 0) {
        return ret;
    }
    // Count at +0x80, then the ids as 4-byte values; the framework reconciles
    // these against what it has stored, so they have to be exactly the ids the
    // enroll reported (both come from the same TA field).
    const uint32_t count = readU32(request, 0x80);
    if (fingerIds != nullptr) {
        fingerIds->clear();
        fingerIds->reserve(count);
        for (uint32_t i = 0; i < count && (0x84 + i * 4 + 4) <= request.size(); ++i) {
            fingerIds->push_back(readU32(request, 0x84 + i * 4));
        }
    }
    return 0;
}

int IFingerprintCore::removeSingleFinger(uint32_t groupId, uint32_t fingerId, uint32_t remaining) {
    auto request = makeRequest(TaCmd::RemoveFinger, 0x80);
    writeU32(request, 0x30, groupId);
    writeU32(request, 0x34, fingerId);
    const int ret = sendTa(request);
    if (ret != 0) {
        LOG(ERROR) << "IFingerprintCore: remove failed: " << ret;
    }
    if (mNotify != nullptr) {
        mNotify->notifyRemove(groupId, fingerId, remaining);
    }
    return ret;
}

int IFingerprintCore::remove(uint32_t groupId, uint32_t fingerId) {
    if (fingerId != 0) {
        return removeSingleFinger(groupId, fingerId, 0);
    }
    std::vector<uint32_t> fingerIds;
    const int ret = enumerate(&fingerIds);
    if (ret != 0) {
        return ret;
    }
    if (fingerIds.empty()) {
        if (mNotify != nullptr) {
            mNotify->notifyRemove(groupId, 0, 0);
        }
        return 0;
    }
    uint32_t remaining = static_cast<uint32_t>(fingerIds.size());
    for (uint32_t id : fingerIds) {
        removeSingleFinger(groupId, id, --remaining);
    }
    return 0;
}

int IFingerprintCore::setWorkMode(WorkMode mode) {
    mWorkMode = mode;
    return 0;
}

int IFingerprintCore::doTouchDownEvent() {
    if (mWorkMode == WorkMode::Enroll) {
        return enrollDown();
    }
    if (mWorkMode == WorkMode::Authenticate) {
        return authenticateDown();
    }
    return 0;
}

int IFingerprintCore::doTouchUpEvent() {
    return 0;
}

void IFingerprintCore::reportError(int32_t error) {
    if (mNotify != nullptr) {
        mNotify->notifyError(error);
    }
}

int IFingerprintCore::cancel() {
    mCancelled = true;
    // The TA has to be told as well: an abort that only stops the HAL locally
    // leaves the algorithm in the aborted operation's state, and every later
    // enroll or authenticate touch is rejected.
    auto request = makeRequest(TaCmd::Cancel, 0x30);
    return sendTa(request);
}

}  // namespace fingerprint
}  // namespace android
