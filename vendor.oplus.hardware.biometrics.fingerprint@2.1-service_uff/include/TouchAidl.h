/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/vendor/oplus/hardware/touch/IOplusTouch.h>

#include <cstdint>
#include <memory>
#include <mutex>

namespace android {
namespace fingerprint {

using ::aidl::vendor::oplus::hardware::touch::IOplusTouch;

/*
 * Thin client for the OPLUS touch service. The panel is routed to the
 * fingerprint driver through a node on that service as well as through
 * /proc/touchpanel/fp_enable, and neither alone is enough.
 */
class TouchAidl {
  public:
    static TouchAidl& instance();

    TouchAidl(const TouchAidl&) = delete;
    TouchAidl& operator=(const TouchAidl&) = delete;

    // Writes `value` to the touch node identified by `nodeFlag`. Returns 0 on
    // success and -1 if the service or the node is unavailable.
    int writeNode(int32_t nodeFlag, int32_t value);

  private:
    TouchAidl() = default;

    std::mutex mMutex;
    std::shared_ptr<::aidl::vendor::oplus::hardware::touch::IOplusTouch> mTouch;
};

}  // namespace fingerprint
}  // namespace android
