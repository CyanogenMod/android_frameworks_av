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
#define LOG_TAG "NdkCameraDevice"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include <utils/Log.h>
#include <utils/Trace.h>

#include <NdkCameraDevice.h>
#include "impl/ACameraDevice.h"

using namespace android;

EXPORT
camera_status_t ACameraDevice_close(ACameraDevice* device) {
    ATRACE_CALL();
    if (device == nullptr) {
        ALOGE("%s: invalid argument! device is null", __FUNCTION__);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    delete device;
    return ACAMERA_OK;
}

EXPORT
const char* ACameraDevice_getId(const ACameraDevice* device) {
    ATRACE_CALL();
    if (device == nullptr) {
        ALOGE("%s: invalid argument! device is null", __FUNCTION__);
        return nullptr;
    }
    return device->getId();
}

EXPORT
camera_status_t ACameraDevice_createCaptureRequest(
        const ACameraDevice* device,
        ACameraDevice_request_template templateId,
        ACaptureRequest** request) {
    ATRACE_CALL();
    if (device == nullptr || request == nullptr) {
        ALOGE("%s: invalid argument! device 0x%p request 0x%p",
                __FUNCTION__, device, request);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    switch (templateId) {
        case TEMPLATE_PREVIEW:
        case TEMPLATE_STILL_CAPTURE:
        case TEMPLATE_RECORD:
        case TEMPLATE_VIDEO_SNAPSHOT:
        case TEMPLATE_ZERO_SHUTTER_LAG:
        case TEMPLATE_MANUAL:
            break;
        default:
            ALOGE("%s: unknown template ID %d", __FUNCTION__, templateId);
            return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    return device->createCaptureRequest(templateId, request);
}

struct ACaptureSessionOutputContainer;

struct ACaptureSessionOutput;

EXPORT
camera_status_t ACaptureSessionOutputContainer_create(/*out*/ACaptureSessionOutputContainer**) {
    ATRACE_CALL();
    return ACAMERA_OK;
}

EXPORT
void ACaptureSessionOutputContainer_free(ACaptureSessionOutputContainer*) {
    ATRACE_CALL();
    return;
}

EXPORT
camera_status_t ACaptureSessionOutput_create(ANativeWindow*, /*out*/ACaptureSessionOutput**) {
    ATRACE_CALL();
    return ACAMERA_OK;
}

EXPORT
void ACaptureSessionOutput_free(ACaptureSessionOutput*) {
    ATRACE_CALL();
    return;
}

EXPORT
camera_status_t ACaptureSessionOutputContainer_add(
        ACaptureSessionOutputContainer*, const ACaptureSessionOutput*) {
    ATRACE_CALL();
    return ACAMERA_OK;
}

EXPORT
camera_status_t ACaptureSessionOutputContainer_remove(
        ACaptureSessionOutputContainer*, const ACaptureSessionOutput*) {
    ATRACE_CALL();
    return ACAMERA_OK;
}

EXPORT
camera_status_t ACameraDevice_createCaptureSession(
        ACameraDevice*,
        const ACaptureSessionOutputContainer*       outputs,
        const ACameraCaptureSession_stateCallbacks* callbacks) {
    ATRACE_CALL();
    return ACAMERA_OK;
}
