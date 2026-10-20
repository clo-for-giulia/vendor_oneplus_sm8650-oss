/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Fingerprint trusted-application (TA) wire protocol.
 *
 * Every request is a single buffer built by the HAL. The first 0x30 bytes are a
 * fixed header; command parameters start at offset 0x30. Field offsets and
 * command ids were recovered from the vendor service (see README.md).
 *
 *   +0x00 u32 module      2 = fingerprint TA
 *   +0x04 u32 cmd         see TaCmd
 *   +0x08 u32              reserved
 *   +0x0c u32              reserved
 *   +0x10 u64 timestamp   ms since local midnight (filled by FpCa)
 *   +0x18 u32 cmd_len     buffer length (filled by FpCa)
 *   +0x1c u32 extra_len   TA log buffer size (filled by FpCa)
 *   +0x20 u32[4]          command specific
 *   +0x30 ...             command parameters
 */

namespace android {
namespace fingerprint {

constexpr uint32_t kModuleFingerprint = 2;
constexpr uint32_t kModuleCalibrate = 5;

// Largest command buffer any TaCmd requires.
constexpr size_t kMaxTaCommandSize = 0x240;

enum class TaCmd : uint32_t {
    Init = 0x7d0,               // 2000  read calibration state / algo version
    SetActiveGroup = 0x7d2,     // 2002
    GetFeature = 0x7d3,         // 2003
    Capture = 0x7d4,            // 2004
    PreEnroll = 0x7d5,          // 2005
    Enroll = 0x7d6,             // 2006  opens the enroll session, carries the auth token
    EnrollTemperature = 0x7d7,  // 2007  temperature hint for the enroll capture
    EnrollImage = 0x7d8,        // 2008
    EnrollSaveTemplate = 0x7d9, // 2009
    GetAuthenticatorId = 0x7db, // 2011
    Authenticate = 0x7dc,       // 2012  opens the authenticate session
    AuthenticateStart = 0x7dd,  // 2013
    AuthCompare = 0x7de,        // 2014
    AuthStudy = 0x7df,          // 2015
    AuthFinish = 0x7e0,         // 2016
    RemoveFinger = 0x7e1,       // 2017
    Enumerate = 0x7e2,          // 2018
    Cancel = 0x7e3,             // 2019  aborts the operation in the TA
    HmacKey = 0x7e4,            // 2020
    TouchUpStatus = 0x8a1,      // 2209  finger-lift status for the running capture
    CalibrateWrite = 0x15,      // write calibration blob (module 5)
    SetBaseConfig = 0x834,      // 2100
    GetConfigField = 0x835,     // 2101
    EnrollFinish = 0x836,       // 2102
};

// Result codes returned by the TA in the capture response.
enum class TaResult : int32_t {
    NeedRetry = 0x13ed,
    NeedRetry2 = 0x13ec,
};

// Status surfaced by the transport when a command could not be delivered.
constexpr int32_t kTaSendFailed = 0x277e;

// TA config field selectors used by TaCmd::GetConfigField.
enum class TaConfigField : uint32_t {
    FpId = 0x0a,
    IconSize = 0x0b,
    IconLocation = 0x0c,
    IconNumber = 0x0d,
    SensorLocation = 0x0e,
    SensorRotation = 0x0f,
    SensorType = 0x10,
    CircleNumber = 0x11,
    TaLogBufLen = 0x31,
    HmacKeyGate = 0x66,
    BrightnessStrategy = 0x3f9,
};

}  // namespace fingerprint
}  // namespace android
