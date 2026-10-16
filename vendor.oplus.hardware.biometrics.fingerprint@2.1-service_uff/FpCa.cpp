/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "FpCa.h"
#include "QseeCa.h"

#include <android-base/logging.h>
#include <sys/time.h>

#include <cstring>
#include <ctime>

namespace android {
namespace fingerprint {

std::string_view taNameToString(TaName name) {
    switch (name) {
        case TaName::UffSpi:
            return "uff_spi";
        case TaName::UffGx:
        case TaName::UffGxAlt:
        case TaName::UffGx2:
            return "uff_gx";
        case TaName::UffJv:
        case TaName::UffJvAlt:
        case TaName::UffJv2:
        case TaName::UffJv3:
        case TaName::UffJv4:
        case TaName::UffJv5:
            return "uff_jv";
        case TaName::UffSilead:
        case TaName::UffSilead2:
            return "uff_silead";
        case TaName::UffEgis:
        case TaName::UffEgis2:
            return "uff_egis";
        case TaName::UffFpc:
            return "uff_fpc";
        case TaName::UffQcom:
            return "uff_qcom";
        case TaName::Max:
            break;
    }
    return {};
}

FpCa::FpCa(std::string_view name) : mName(name) {}

int FpCa::startTa(TaName name) {
    const int ret = doStartTa(name);
    mStarted = (ret == 0);
    return ret;
}

int FpCa::closeTa() {
    if (!mStarted) {
        return 0;
    }
    const int ret = doCloseTa();
    mStarted = false;
    return ret;
}

int FpCa::sendCommand(std::span<uint8_t> buffer) {
    if (buffer.empty()) {
        return -1;
    }
    if (!mStarted) {
        LOG(ERROR) << "FpCa: TA not loaded";
        return 0x2784;
    }

    // Fill the header fields owned by the transport layer.
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    struct tm tm = {};
    localtime_r(&tv.tv_sec, &tm);
    const uint64_t timestamp =
            (static_cast<uint64_t>(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec) * 1000ULL) +
            static_cast<uint64_t>(tv.tv_usec / 1000);

    const uint32_t length = static_cast<uint32_t>(buffer.size());
    memcpy(buffer.data() + 0x10, &timestamp, sizeof(timestamp));
    memcpy(buffer.data() + 0x18, &length, sizeof(length));
    memcpy(buffer.data() + 0x1c, &mExtraLength, sizeof(mExtraLength));

    return doSendCommand(buffer);
}

FpCa* createCa(std::string_view name) {
    if (name == "qsee") {
        return new QseeCa();
    }
    LOG(ERROR) << "createCa: unsupported CA backend '" << name << "'";
    return {};
}

}  // namespace fingerprint
}  // namespace android
