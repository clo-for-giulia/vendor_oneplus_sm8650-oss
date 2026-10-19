/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FpCa.h"
#include "QseeComApi.h"

#include <cstdint>
#include <mutex>

namespace android {
namespace fingerprint {

// QSEECom back end: transports fingerprint commands to the trusted application
// through the platform's QSEECom userspace API.
class QseeCa : public FpCa {
  public:
    QseeCa();
    ~QseeCa() override;

    int sendHmacKeyToFpta() override;

  protected:
    int doStartTa(TaName name) override;
    int doCloseTa() override;
    int doSendCommand(std::span<uint8_t> buffer) override;

  private:
    // Grows the cached payload buffer when a command needs more room. Kept
    // between commands because allocating one per round trip means an
    // allocation, an mmap and their teardown every time.
    int managePayloadBuffer(uint32_t size);
    void freePayloadBuffer();

    qsee::QSEECom_handle* mHandle = nullptr;
    // Serializes the shared QSEECom handle: Cancel arrives on a binder thread
    // while a capture may be running on the device thread.
    std::mutex mMutex;
    int mPayloadFd = -1;
    void* mPayloadMap = nullptr;
    uint32_t mPayloadSize = 0;
};

}  // namespace fingerprint
}  // namespace android
