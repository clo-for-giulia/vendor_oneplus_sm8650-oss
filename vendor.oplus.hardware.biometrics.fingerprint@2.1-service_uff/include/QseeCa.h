/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FpCa.h"
#include "QseeComApi.h"

#include <cstdint>

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
    qsee::QSEECom_handle* mHandle = nullptr;
};

}  // namespace fingerprint
}  // namespace android
