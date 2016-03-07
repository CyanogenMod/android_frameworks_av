/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "NdkCaptureRequest"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include <utils/Log.h>
#include <utils/Trace.h>

#include "NdkCaptureRequest.h"
#include "impl/ACameraMetadata.h"
#include "impl/ACaptureRequest.h"

EXPORT
camera_status_t ACameraOutputTarget_create(
        ANativeWindow* window, ACameraOutputTarget** out) {
    ATRACE_CALL();
    if (window == nullptr) {
        ALOGE("%s: Error: input window is null", __FUNCTION__);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    *out = new ACameraOutputTarget(window);
    return ACAMERA_OK;
}

EXPORT
void ACameraOutputTarget_free(ACameraOutputTarget* target) {
    ATRACE_CALL();
    if (target != nullptr) {
        delete target;
    }
    return;
}

EXPORT
camera_status_t ACaptureRequest_addTarget(
        ACaptureRequest* req, const ACameraOutputTarget* target) {
    ATRACE_CALL();
    if (req == nullptr || req->targets == nullptr || target == nullptr) {
        ALOGE("%s: Error: invalid input: req %p, req-targets %p, target %p",
                __FUNCTION__, req, req->targets, target);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    auto pair = req->targets->mOutputs.insert(*target);
    if (!pair.second) {
        ALOGW("%s: target %p already exists!", __FUNCTION__, target);
    }
    return ACAMERA_OK;
}

EXPORT
camera_status_t ACaptureRequest_removeTarget(
        ACaptureRequest* req, const ACameraOutputTarget* target) {
    ATRACE_CALL();
    if (req == nullptr || req->targets == nullptr || target == nullptr) {
        ALOGE("%s: Error: invalid input: req %p, req-targets %p, target %p",
                __FUNCTION__, req, req->targets, target);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    req->targets->mOutputs.erase(*target);
    return ACAMERA_OK;
}

EXPORT
camera_status_t ACaptureRequest_getConstEntry(
        const ACaptureRequest* req, uint32_t tag, ACameraMetadata_const_entry* entry) {
    ATRACE_CALL();
    if (req == nullptr || entry == nullptr) {
        ALOGE("%s: invalid argument! req 0x%p, tag 0x%x, entry 0x%p",
               __FUNCTION__, req, tag, entry);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    return req->settings->getConstEntry(tag, entry);
}

EXPORT
camera_status_t ACaptureRequest_getAllTags(
        const ACaptureRequest* req, /*out*/int32_t* numTags, /*out*/const uint32_t** tags) {
    ATRACE_CALL();
    if (req == nullptr || numTags == nullptr || tags == nullptr) {
        ALOGE("%s: invalid argument! request %p, numTags %p, tags %p",
               __FUNCTION__, req, numTags, tags);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    return req->settings->getTags(numTags, tags);
}

#define SET_ENTRY(NAME,NDK_TYPE)                                                        \
EXPORT                                                                                  \
camera_status_t ACaptureRequest_setEntry_##NAME(                                        \
        ACaptureRequest* req, uint32_t tag, uint32_t count, const NDK_TYPE* data) {     \
    ATRACE_CALL();                                                                      \
    if (req == nullptr || (count > 0 && data == nullptr)) {                             \
        ALOGE("%s: invalid argument! req %p, tag 0x%x, count %d, data 0x%p",            \
               __FUNCTION__, req, tag, count, data);                                    \
        return ACAMERA_ERROR_INVALID_PARAMETER;                                         \
    }                                                                                   \
    return req->settings->update(tag, count, data);                                     \
}

SET_ENTRY(u8,uint8_t)
SET_ENTRY(i32,int32_t)
SET_ENTRY(float,float)
SET_ENTRY(double,double)
SET_ENTRY(i64,int64_t)
SET_ENTRY(rational,ACameraMetadata_rational)

#undef SET_ENTRY

EXPORT
void ACaptureRequest_free(ACaptureRequest* request) {
    ATRACE_CALL();
    if (request == nullptr) {
        return;
    }
    delete request->settings;
    delete request->targets;
    delete request;
    return;
}
