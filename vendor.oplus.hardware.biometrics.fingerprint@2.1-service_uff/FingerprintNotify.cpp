/*
 * Copyright (C) 2026 The YAAP Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "FingerprintNotify.h"

namespace android {
namespace fingerprint {

void FingerprintNotify::emit(const FingerprintMsg& message) {
    if (mCallback) {
        mCallback(message);
    }
}

void FingerprintNotify::notifyAcquiredInfo(int32_t info) {
    FingerprintMsg message;
    message.type = FingerprintMsg::kTypeAcquired;
    message.acquired = info;
    emit(message);
}

void FingerprintNotify::notifyError(int32_t error) {
    FingerprintMsg message;
    message.type = FingerprintMsg::kTypeError;
    message.acquired = error;
    emit(message);
}

void FingerprintNotify::notifyEnrollResult(uint32_t fid, uint32_t gid, int32_t remaining) {
    FingerprintMsg message;
    message.type = FingerprintMsg::kTypeEnroll;
    message.gid = gid;
    message.fid = fid;
    message.remaining = remaining;
    emit(message);
}

void FingerprintNotify::notifyAuthResult(uint32_t fid, uint32_t gid) {
    FingerprintMsg message;
    message.type = FingerprintMsg::kTypeAuthenticated;
    message.gid = gid;
    message.fid = fid;
    emit(message);
}

void FingerprintNotify::notifyInteractionDetected() {
    FingerprintMsg message;
    message.type = FingerprintMsg::kTypeInteractionDetected;
    emit(message);
}

void FingerprintNotify::notifyRemove(uint32_t gid, uint32_t fid, uint32_t remaining) {
    FingerprintMsg message;
    message.type = FingerprintMsg::kTypeRemoved;
    message.gid = gid;
    message.fid = fid;
    message.remaining = static_cast<int32_t>(remaining);
    emit(message);
}

void FingerprintNotify::checkAcquiredInfo(int32_t code) {
    int32_t acquired = 0;
    bool report = true;
    switch (code) {
        case 0x1450:
            acquired = 0x3e9;
            break;
        case 0x1451:
            acquired = 0x3ea;
            break;
        case 0x1452:
            notifyError(FingerprintMsg::kErrorUnableToProcess);
            return;
        case 0x14b6:
            acquired = 1;
            break;
        case 0x14b7:
        case 0x14b8:
        case 0x15e4:
            acquired = 3;
            break;
        case 0x14b9:
        case 0x15e0:
        case 0x277c:
            acquired = 5;
            break;
        case 0x14bc:
            acquired = 0x67;
            break;
        case 0x15e1:
            acquired = 0x457;
            break;
        case 0x15e2:
            acquired = 0x458;
            break;
        case 0x15e3:
            acquired = 0x459;
            break;
        case 0x15e5:
        case 0x15e7:
            acquired = 1;
            break;
        case 0x15e6:
            acquired = 0x45c;
            break;
        default:
            report = false;
            break;
    }
    if (report) {
        notifyAcquiredInfo(acquired);
    }
}

}  // namespace fingerprint
}  // namespace android
