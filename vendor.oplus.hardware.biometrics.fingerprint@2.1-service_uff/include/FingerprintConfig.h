/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string>

namespace android {
namespace fingerprint {

// Configuration resolved from system properties and the persist partition.
class FingerprintConfig {
  public:
    FingerprintConfig() = default;

    int init();

    // Sends the TA base configuration and queries the TA config fields. Must
    // run after the CA has started the TA.
    int taConfigInit(class FpCa* ca);

    // Returns an android.hardware.biometrics.fingerprint.FingerprintSensorType
    // ordinal (0=UNKNOWN, 2=UNDER_DISPLAY_ULTRASONIC, 3=UNDER_DISPLAY_OPTICAL,
    // 4=POWER_BUTTON, 5=HOME_BUTTON).
    int32_t sensorTypeEnum() const;

    int32_t sensorX() const { return mSensorX; }
    int32_t sensorY() const { return mSensorY; }
    int32_t sensorRadius() const { return mSensorRadius; }

    std::string version() const { return mVersion; }
    std::string sensorType() const { return mSensorType; }

    // TA config flags queried from the TA at init.
    bool hmacKeyGate() const { return mHmacKeyGate; }

    std::string factoryAlgoVersion() const { return mFactoryAlgoVersion; }
    std::string baseInfo() const { return mBaseInfo; }

  private:
    int32_t mSensorX = 0;
    int32_t mSensorY = 0;
    int32_t mSensorRadius = 0;

    std::string mVersion;
    std::string mSensorType;

    bool mHmacKeyGate = false;

    std::string mFactoryAlgoVersion;
    std::string mBaseInfo;
};

}  // namespace fingerprint
}  // namespace android
