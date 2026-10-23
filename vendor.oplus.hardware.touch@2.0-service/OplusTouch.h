/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/vendor/oplus/hardware/touch/BnOplusTouch.h>
#include <aidl/vendor/oplus/hardware/touch/IOplusTouchEventCallback.h>
#include <aidl/vendor/oplus/hardware/touch/OplusTouchInfo.h>
#include <aidl/vendor/oplus/hardware/touch/OplusTouchStatus.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aidl::vendor::oplus::hardware::touch {

class OplusTouch : public BnOplusTouch {
  public:
    OplusTouch() = default;
    ~OplusTouch() override = default;

    ndk::ScopedAStatus initialize(OplusTouchStatus* _aidl_return) override;

    ndk::ScopedAStatus isTouchNodeSupport(int32_t in_deviceId, int32_t in_nodeFlag,
                                          int32_t* _aidl_return) override;

    ndk::ScopedAStatus touchReadNodeFile(int32_t in_deviceId, int32_t in_nodeFlag,
                                         std::string* _aidl_return) override;

    ndk::ScopedAStatus touchWriteNodeFile(int32_t in_deviceId, int32_t in_nodeFlag,
                                          const std::string& in_info,
                                          int32_t* _aidl_return) override;

    ndk::ScopedAStatus touchWriteBtInfo(int32_t in_deviceId, int32_t in_nodeFlag,
                                        const std::string& in_info,
                                        int32_t* _aidl_return) override;

    ndk::ScopedAStatus touchWriteNodeFileOneWay(int32_t in_deviceId, int32_t in_nodeFlag,
                                                const std::string& in_info) override;

    ndk::ScopedAStatus touchNotifyClient(int32_t in_clientFlag,
                                         const OplusTouchInfo& in_info,
                                         OplusTouchStatus* _aidl_return) override;

    ndk::ScopedAStatus registerEventCallback(
            const std::shared_ptr<IOplusTouchEventCallback>& in_callback,
            OplusTouchStatus* _aidl_return) override;

    ndk::ScopedAStatus unregisterEventCallback(
            const std::shared_ptr<IOplusTouchEventCallback>& in_callback,
            OplusTouchStatus* _aidl_return) override;

  private:
    int32_t writeNodeFile(int32_t deviceId, int32_t nodeFlag, const std::string& info);

    // The HBP kernel does not expose the legacy double_tap_enable_indep proc
    // node. Keep the AIDL node contract usable by exposing the gesture mask
    // that the Lineage touch HAL programs through this service. This is a
    // bookkeeping mask; per-gesture actions are gated by the touch HAL/app.
    std::atomic<uint32_t> mGestureMask{0};
    std::atomic<bool> mDoubleTapEnabled{true};

    std::mutex mCallbackMutex;
    std::vector<std::shared_ptr<IOplusTouchEventCallback>> mCallbacks;
};

}  // namespace aidl::vendor::oplus::hardware::touch
