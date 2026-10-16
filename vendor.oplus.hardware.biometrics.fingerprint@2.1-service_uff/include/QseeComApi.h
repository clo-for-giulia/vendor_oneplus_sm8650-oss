/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Declarations for the platform QSEECom userspace API.
 *
 * On this platform the TEE is reached through Qualcomm's TZCom/Mink stack
 * (smcinvoke); `libQSEEComAPI` is the supported entry point and is provided by
 * the device's vendor partition. Only the calls the HAL needs are declared.
 */

#include <cstdint>

namespace android {
namespace fingerprint {
namespace qsee {

// Trusted-application handle. `ion_sbuffer` is the shared command/response
// buffer allocated by QSEECom_start_app().
struct QSEECom_handle {
    void* ion_sbuffer;
    uint32_t sbuffer_len;
};

struct IonFdData {
    int32_t fd;
    uint32_t cmdBufOffset;
};

// Up to four externally allocated dma-bufs may be attached to a command.
struct IonFdInfo {
    IonFdData data[4];
};

}  // namespace qsee
}  // namespace fingerprint
}  // namespace android

extern "C" {

int QSEECom_start_app(android::fingerprint::qsee::QSEECom_handle** handle, const char* path,
                      const char* appName, uint32_t size);
int QSEECom_shutdown_app(android::fingerprint::qsee::QSEECom_handle** handle);
int QSEECom_send_cmd(android::fingerprint::qsee::QSEECom_handle* handle, void* req,
                     uint32_t reqLen, void* resp, uint32_t respLen);
int QSEECom_send_modified_cmd(android::fingerprint::qsee::QSEECom_handle* handle, void* req,
                              uint32_t reqLen, void* resp, uint32_t respLen,
                              android::fingerprint::qsee::IonFdInfo* ionFdInfo);

}  // extern "C"
