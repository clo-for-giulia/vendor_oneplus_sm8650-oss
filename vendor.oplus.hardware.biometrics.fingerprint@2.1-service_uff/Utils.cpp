/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Utils.h"

#include <android-base/properties.h>

#include <ctime>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace android {
namespace fingerprint {

namespace {
/*
 * The proprietary service ran with BiometricsFingerprintShim interposed, which
 * overrode __system_property_get()/property_get() process-wide and forced these
 * three reads. The TA base configuration embeds ro.boot.vbmeta.device_state, so
 * the TA has to see the same boot state it saw there.
 */
constexpr std::pair<std::string_view, std::string_view> kForcedProperties[] = {
        {"ro.boot.vbmeta.device_state", "unlocked"},
        {"ro.boot.verifiedbootstate", "orange"},
        {"ro.boot.mode", "normal"},
};
}  // namespace

std::string Utils::getProperty(const std::string& key, const std::string& defaultValue) {
    for (const auto& [name, value] : kForcedProperties) {
        if (key == name) {
            return std::string(value);
        }
    }
    return ::android::base::GetProperty(key, defaultValue);
}

int32_t Utils::getPropertyInt(const std::string& key, int32_t defaultValue) {
    return ::android::base::GetIntProperty(key, defaultValue);
}

bool Utils::setProperty(const std::string& key, const std::string& value) {
    return ::android::base::SetProperty(key, value);
}

bool Utils::isFtmMode() {
    // `sys.oplus_ftm_mode` is set by the bootloader on factory test units.
    return getPropertyInt("sys.oplus_ftm_mode", 0) != 0;
}

bool Utils::isMetaMode() {
    return getProperty("ro.boot.mode", "") == "meta";
}

bool Utils::isFileExist(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

uint64_t Utils::getTimeUs() {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

}  // namespace fingerprint
}  // namespace android
