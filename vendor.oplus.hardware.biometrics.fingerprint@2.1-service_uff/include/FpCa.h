/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FpTypes.h"
#include "TaProtocol.h"

#include <cstdint>
#include <span>
#include <string>

namespace android {
namespace fingerprint {

// Firmware search paths, tried in order. Recovered from the vendor service.
constexpr const char* kTaFirmwarePaths[] = {
        "/odm/vendor/firmware",
        "/odm/firmware",
        "/vendor/firmware",
};

// Client application (CA) that owns a trusted-application session and
// transports fingerprint commands to it.
class FpCa {
  public:
    explicit FpCa(std::string_view name);
    virtual ~FpCa() = default;

    // Loads the TA identified by `name`. Returns 0 on success.
    int startTa(TaName name);
    // Tears the TA session down.
    int closeTa();

    // Frames and sends a command buffer. On success the response is written
    // back into `buffer` and 0 is returned; otherwise a negative/status code.
    int sendCommand(std::span<uint8_t> buffer);

    // Forwards the HMAC key from the keymaster TA to the fingerprint TA.
    virtual int sendHmacKeyToFpta() = 0;

    // Size of the trailing TA log buffer, read from the TA config.
    void setExtraLength(uint32_t length) { mExtraLength = length; }

    const std::string& name() const { return mName; }

  protected:
    virtual int doStartTa(TaName name) = 0;
    virtual int doCloseTa() = 0;
    virtual int doSendCommand(std::span<uint8_t> buffer) = 0;

    std::string mName;
    bool mStarted = false;
    uint32_t mExtraLength = 0;
};

// Creates the CA implementation selected by `name` ("qsee"). Returns nullptr
// for an unknown backend.
[[nodiscard]] FpCa* createCa(std::string_view name);

}  // namespace fingerprint
}  // namespace android
