/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace aidl::vendor::oplus::hardware::touch {

// The stock service treats this nodeFlag as "list every supported node".
constexpr int32_t kTouchNodeListFlag = 0xa8;

struct TouchNode {
    int32_t flag;
    int32_t devicePosition;
    const char* directory;
    const char* name;
    size_t readSize;
};

// Resolve the path exactly like the stock getNodeFilePath() table walker.
// devicePosition 0 is not used by this table; 1 inserts the device ID before
// the final slash, and 2 appends it to the node name.
std::optional<std::string> getTouchNodePath(int32_t deviceId, int32_t nodeFlag,
                                            size_t* readSize);

// Reproduce the special 0xa8 listing operation used by touchHidlTest.
std::string listTouchNodes(int32_t deviceId);

}  // namespace aidl::vendor::oplus::hardware::touch
