/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "FingerprintConfig.h"

#include "ByteBuffer.h"
#include "FpCa.h"
#include "TaProtocol.h"
#include "Utils.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace android {
namespace fingerprint {

namespace {
constexpr char kCaliSerialPath[] = "/mnt/vendor/persist/fingerprint/fp_cali_serial_number.txt";
constexpr char kLcdInfoPath[] = "/proc/devinfo/lcd";
constexpr char kHalVersion[] = "UFF V1.0.0.0";

/*
 * Layout of the TA base configuration (cmd 0x834). The TA reads the project,
 * panel and lock-state identifiers as fixed 32-byte fields. +0x20 carries a
 * single state byte that the vendor only ever fills from a TA query performed
 * after this command, and +0x70 is never written on any path, so both are sent
 * zero exactly as the vendor does on the first configuration.
 */
constexpr size_t kBaseConfigSize = 0xb0;
constexpr size_t kBaseConfigProjectOffset = 0x30;
constexpr size_t kBaseConfigLcdOffset = 0x50;
constexpr size_t kBaseConfigLockStateOffset = 0x90;
constexpr size_t kBaseConfigFieldSize = 0x20;
// The TA's log area travels with every command. Keep enough room for a short
// diagnostic line, but do not make every capture/compare carry the full
// configured log buffer unless talog tracing is explicitly enabled.
constexpr uint32_t kCompactTaLogBufferSize = 256;

// Copies a string into one of the base configuration's fixed 32-byte fields.
void writeField(std::vector<uint8_t>& buffer, size_t offset, const std::string& value) {
    const size_t length = std::min(value.size(), kBaseConfigFieldSize);
    if (offset + length <= buffer.size()) {
        memcpy(buffer.data() + offset, value.data(), length);
    }
}

// Reads the NUL-terminated value of a config-field response (offset 0x38).
std::string fieldString(const std::vector<uint8_t>& buffer, size_t maxLength = 0x20) {
    const char* text = reinterpret_cast<const char*>(buffer.data() + 0x38);
    return std::string(text, strnlen(text, maxLength));
}

void publishProperty(const char* key, const std::string& value) {
    if (!value.empty() && !Utils::setProperty(key, value)) {
        LOG(WARNING) << "FingerprintConfig: failed to set " << key;
    }
}

/*
 * Panel identifier the TA matches its per-panel data on: the uppercased
 * /proc/devinfo/lcd contents starting at the first recognised panel keyword.
 * A SAMSUNG panel is identified by its model rather than by the vendor name.
 */
std::string readLcdType() {
    std::ifstream in(kLcdInfoPath, std::ios::binary);
    if (!in.is_open()) {
        return {};
    }

    char raw[0x80] = {};
    in.read(raw, sizeof(raw));
    std::string info(raw, static_cast<size_t>(in.gcount()));
    std::transform(info.begin(), info.end(), info.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    size_t pos = info.find("BOE");
    if (pos == std::string::npos) {
        pos = info.find("P_3");
    }
    if (pos == std::string::npos) {
        if (info.find("SAMSUNG") != std::string::npos) {
            pos = info.find("S6E3HC4");
            if (pos == std::string::npos) {
                pos = info.find("AMB670YF");
            }
        } else {
            for (const char* keyword : {"P_1", "TM", "TIANMA", "P_7", "VISIONOX", "P_B", "VXN"}) {
                pos = info.find(keyword);
                if (pos != std::string::npos) {
                    break;
                }
            }
        }
    }
    return pos == std::string::npos ? std::string() : info.substr(pos);
}

// The TA keys its per-project data off this. ro.separate.soft is authoritative;
// ro.boot.prjname is only a fallback for when it is absent or a placeholder.
std::string readProjectName() {
    std::string project = Utils::getProperty("ro.separate.soft", "");
    if (project.empty() || project.front() == '0') {
        project = Utils::getProperty("ro.boot.prjname", project);
    }
    return project;
}
}  // namespace

int FingerprintConfig::init() {
    mSensorType = Utils::getProperty("persist.vendor.fingerprint.sensor_type", "");

    const auto location =
            Utils::getProperty("persist.vendor.fingerprint.optical.sensorlocation", "");
    if (!location.empty()) {
        // The TA separates the pair with a double colon ("720::2353"); Tokenize
        // skips the empty field, so Y is the second token.
        const auto parts = android::base::Tokenize(location, ":");
        if (parts.size() >= 2) {
            mSensorX = std::atoi(parts[0].c_str());
            mSensorY = std::atoi(parts[1].c_str());
        } else {
            LOG(WARNING) << "FingerprintConfig: invalid sensorlocation " << location;
        }
    }

    const auto size = Utils::getProperty("persist.vendor.fingerprint.optical.iconsize", "");
    if (!size.empty()) {
        mSensorRadius = std::atoi(size.c_str()) / 2;
    }

    // The vendor publishes the HAL version for the rest of the stack to read;
    // nothing else writes this property.
    mVersion = kHalVersion;
    if (!Utils::setProperty("persist.vendor.fingerprint.version", mVersion)) {
        LOG(WARNING) << "FingerprintConfig: failed to set persist.vendor.fingerprint.version";
    }

    if (Utils::isFileExist(kCaliSerialPath)) {
        mFactoryAlgoVersion = "calibrated";
    }

    mBaseInfo = Utils::getProperty("ro.vendor.oplus.version", "");
    return 0;
}

int32_t FingerprintConfig::sensorTypeEnum() const {
    // Ordering follows FingerprintSensorType.
    if (mSensorType == "ultrasonic") {
        return 2;  // UNDER_DISPLAY_ULTRASONIC
    }
    if (mSensorType == "optical") {
        return 3;  // UNDER_DISPLAY_OPTICAL
    }
    if (mSensorType == "side") {
        return 4;  // POWER_BUTTON
    }
    if (mSensorType == "front") {
        return 5;  // HOME_BUTTON
    }
    if (mSensorType == "back") {
        return 1;  // REAR
    }
    return 0;  // UNKNOWN
}

int FingerprintConfig::taConfigInit(FpCa* ca) {
    if (ca == nullptr) {
        return -1;
    }

    // Base configuration (cmd 0x834, module 2). The TA identifies the panel from
    // the project and LCD fields, and takes the lock state as the raw
    // ro.boot.vbmeta.device_state string rather than as a boolean.
    std::vector<uint8_t> base(kBaseConfigSize, 0);
    writeU32(base, 0x00, kModuleFingerprint);
    writeU32(base, 0x04, static_cast<uint32_t>(TaCmd::SetBaseConfig));

    writeField(base, kBaseConfigProjectOffset, readProjectName());
    writeField(base, kBaseConfigLcdOffset, readLcdType());
    writeField(base, kBaseConfigLockStateOffset,
               Utils::getProperty("ro.boot.vbmeta.device_state", ""));

    const int ret = ca->sendCommand(base);
    if (ret != 0) {
        LOG(ERROR) << "FingerprintConfig: base config failed: " << ret;
        return ret;
    }

    // Query the individual TA configuration fields (cmd 0x835).
    auto query = [ca](TaConfigField selector, uint32_t length, std::vector<uint8_t>* out) -> int {
        std::vector<uint8_t> buffer(0x240, 0);
        writeU32(buffer, 0x00, kModuleFingerprint);
        writeU32(buffer, 0x04, static_cast<uint32_t>(TaCmd::GetConfigField));
        writeU32(buffer, 0x30, static_cast<uint32_t>(selector));
        writeU32(buffer, 0x238, length);
        const int result = ca->sendCommand(buffer);
        *out = std::move(buffer);
        return result;
    };

    // These lengths are the TA's own field sizes; asking for more is rejected
    // with a non-zero status.
    std::vector<uint8_t> response;
    if (query(TaConfigField::TaLogBufLen, 4, &response) == 0) {
        const uint32_t logLength = readU32(response, 0x38);
        const bool keepTaLog =
                android::base::GetBoolProperty("persist.vendor.fingerprint.talog", false);
        const uint32_t usedLength =
                keepTaLog ? logLength : std::min(logLength, kCompactTaLogBufferSize);
        LOG(INFO) << "FingerprintConfig: TA log buffer " << logLength << " bytes, using "
                  << usedLength;
        ca->setExtraLength(usedLength);
    } else {
        LOG(WARNING) << "FingerprintConfig: TA log buffer query failed";
    }
    if (query(TaConfigField::HmacKeyGate, 4, &response) == 0) {
        mHmacKeyGate = readU32(response, 0x38) != 0;
    }
    if (query(TaConfigField::BrightnessStrategy, 0x20, &response) == 0) {
        publishProperty("persist.vendor.fingerprint.brightness.strategy", fieldString(response));
    }

    // The TA also carries the sensor identity and geometry. Publish it the way
    // the vendor's FingerprintConfig::setIconType() does, and apply the same
    // mapping the LineageOS SensorProps shim used, so the reported geometry is
    // correct even when the persist partition holds no stock values.
    std::string sensorType;
    if (query(TaConfigField::SensorType, 0x20, &response) == 0) {
        sensorType = fieldString(response);
    }
    if (!sensorType.empty()) {
        mSensorType = sensorType;
    }
    publishProperty("persist.vendor.fingerprint.sensor_type", sensorType);

    std::string fpId;
    if (query(TaConfigField::FpId, 0x20, &response) == 0) {
        fpId = fieldString(response);
    }
    publishProperty("persist.vendor.fingerprint.fp_id", fpId);

    if (mSensorType == "optical" || mSensorType == "ultrasonic") {
        std::string iconSize;
        std::string iconLocation;
        std::string iconNumber;
        std::string sensorLocation;
        std::string sensorRotation;
        std::string circleNumber;
        if (query(TaConfigField::IconSize, 0x20, &response) == 0) {
            iconSize = fieldString(response);
        }
        if (query(TaConfigField::IconLocation, 0x20, &response) == 0) {
            iconLocation = fieldString(response);
        }
        if (query(TaConfigField::IconNumber, 0x20, &response) == 0) {
            iconNumber = fieldString(response);
        }
        if (query(TaConfigField::SensorLocation, 0x20, &response) == 0) {
            sensorLocation = fieldString(response);
        }
        if (query(TaConfigField::SensorRotation, 0x20, &response) == 0) {
            sensorRotation = fieldString(response);
        }
        if (query(TaConfigField::CircleNumber, 0x20, &response) == 0) {
            circleNumber = fieldString(response);
        }

        publishProperty("persist.vendor.fingerprint.optical.iconsize", iconSize);
        publishProperty("persist.vendor.fingerprint.optical.iconlocation", iconLocation);
        publishProperty("persist.vendor.fingerprint.optical.iconnumber", iconNumber);
        publishProperty("persist.vendor.fingerprint.optical.sensorlocation", sensorLocation);
        publishProperty("persist.vendor.fingerprint.optical.sensorrotation", sensorRotation);
        publishProperty("persist.vendor.fingerprint.optical.circlenumber", circleNumber);

        const auto parts = android::base::Tokenize(sensorLocation, ":");
        if (parts.size() >= 2) {
            mSensorX = std::atoi(parts[0].c_str());
            mSensorY = std::atoi(parts[1].c_str());
        }
        if (!iconSize.empty()) {
            mSensorRadius = std::atoi(iconSize.c_str()) / 2;
        }
    }
    return 0;
}

}  // namespace fingerprint
}  // namespace android
