/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "BiometricsFingerprintAidl.h"

#include "BiometricsFpSession.h"
#include "FingerprintConfig.h"
#include "FpCa.h"
#include "IFingerprintCore.h"
#include "Utils.h"

#include <aidl/android/hardware/biometrics/common/CommonProps.h>
#include <aidl/android/hardware/biometrics/common/ComponentInfo.h>
#include <aidl/android/hardware/biometrics/common/SensorStrength.h>
#include <aidl/android/hardware/biometrics/fingerprint/SensorLocation.h>

#include <unistd.h>

using aidl::android::hardware::biometrics::common::CommonProps;
using aidl::android::hardware::biometrics::common::ComponentInfo;
using aidl::android::hardware::biometrics::common::SensorStrength;
using aidl::android::hardware::biometrics::fingerprint::FingerprintSensorType;
using aidl::android::hardware::biometrics::fingerprint::SensorLocation;

namespace android {
namespace fingerprint {

BiometricsFingerprintAidl::BiometricsFingerprintAidl() : mContext(std::make_unique<HalContext>()) {}

BiometricsFingerprintAidl::~BiometricsFingerprintAidl() {
    if (mContext != nullptr) {
        mContext->deinit();
    }
}

int BiometricsFingerprintAidl::init() {
    if (mInitialized) {
        return 0;
    }
    if (mContext->init() != 0) {
        return -1;
    }
    mInitialized = true;
    return 0;
}

void BiometricsFingerprintAidl::fillComponentInfo(SensorProps* props) {
    FingerprintConfig* config = mContext->config();

    ComponentInfo fingerprint = {};
    fingerprint.componentId = "fingerprint";
    fingerprint.hardwareVersion = Utils::getProperty("ro.boot.prjname", "unknown");
    fingerprint.firmwareVersion = config != nullptr ? config->version() : "";
    fingerprint.serialNumber = config != nullptr ? config->baseInfo() : "";
    fingerprint.softwareVersion = Utils::getProperty("ro.vendor.oplus.version", "");

    ComponentInfo algorithm = {};
    algorithm.componentId = "Algorithm matching";
    algorithm.hardwareVersion = config != nullptr ? config->factoryAlgoVersion() : "";
    algorithm.firmwareVersion = config != nullptr ? config->version() : "";
    algorithm.serialNumber = "";
    algorithm.softwareVersion = "";

    props->commonProps.componentInfo = {fingerprint, algorithm};
}

ndk::ScopedAStatus BiometricsFingerprintAidl::getSensorProps(std::vector<SensorProps>* out) {
    if (out == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    FingerprintConfig* config = mContext->config();
    const int32_t sensorTypeEnum = config != nullptr ? config->sensorTypeEnum() : 0;

    SensorProps props = {};
    props.commonProps.sensorId = 0;
    // Class 3, as the vendor reports it. CONVENIENCE is not exposed through
    // the public biometric API at all, so apps could not use the sensor.
    props.commonProps.sensorStrength = SensorStrength::STRONG;
    props.commonProps.maxEnrollmentsPerUser = 5;
    fillComponentInfo(&props);

    props.sensorType = static_cast<FingerprintSensorType>(sensorTypeEnum);

    SensorLocation location = {};
    location.displayId = 0;
    location.sensorLocationX = config != nullptr ? config->sensorX() : 0;
    location.sensorLocationY = config != nullptr ? config->sensorY() : 0;
    location.sensorRadius = config != nullptr ? config->sensorRadius() : 0;
    props.sensorLocations = {location};

    props.supportsNavigationGestures = false;
    props.supportsDetectInteraction = true;

    // The HAL reads the display touches from the driver itself; illumination is
    // the framework's — its UDFPS dim layer is what the display stack turns
    // into panel HBM — so it must be told the HAL does not control it.
    const bool udfps = props.sensorType == FingerprintSensorType::UNDER_DISPLAY_OPTICAL ||
                       props.sensorType == FingerprintSensorType::UNDER_DISPLAY_ULTRASONIC;
    props.halHandlesDisplayTouches = udfps;
    props.halControlsIllumination = false;

    out->push_back(props);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BiometricsFingerprintAidl::createSession(
        int32_t sensorId, int32_t userId, const std::shared_ptr<ISessionCallback>& callback,
        std::shared_ptr<ISession>* out) {
    if (out == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    auto session = ndk::SharedRefBase::make<BiometricsFpSession>(mContext.get(), sensorId, userId,
                                                                 callback);
    if (session->init() != 0) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    *out = session;
    return ndk::ScopedAStatus::ok();
}

binder_status_t BiometricsFingerprintAidl::dump(int fd, const char** args, uint32_t numArgs) {
    (void)args;
    (void)numArgs;
    if (fd < 0) {
        return STATUS_BAD_VALUE;
    }
    FingerprintConfig* config = mContext->config();
    std::string report;
    report += "OPLUS UFF fingerprint HAL\n";
    report += "sensor type: " + (config != nullptr ? config->sensorType() : std::string()) + "\n";
    report += "ca backend: " + mContext->ca()->name() + "\n";
    report += "ta: " + std::string(taNameToString(mContext->selectedTa())) + "\n";
    ::write(fd, report.c_str(), report.size());
    return STATUS_OK;
}

}  // namespace fingerprint
}  // namespace android
