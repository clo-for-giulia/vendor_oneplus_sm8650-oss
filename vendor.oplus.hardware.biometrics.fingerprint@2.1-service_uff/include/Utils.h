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

// Small stateless helpers shared by the HAL.
class Utils {
  public:
    Utils() = delete;

    static std::string getProperty(const std::string& key, const std::string& defaultValue = "");
    static int32_t getPropertyInt(const std::string& key, int32_t defaultValue = 0);
    static bool setProperty(const std::string& key, const std::string& value);

    static bool isFtmMode();
    static bool isMetaMode();
    static bool isFileExist(const std::string& path);

    // Monotonic time in microseconds.
    static uint64_t getTimeUs();
};

}  // namespace fingerprint
}  // namespace android
