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
#include "impl/ACameraCaptureSession.h"

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
        ALOGE("%s: invalid argument! device %p request %p",
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

EXPORT
camera_status_t ACaptureSessionOutputContainer_create(
        /*out*/ACaptureSessionOutputContainer** out) {
    ATRACE_CALL();
    if (out == nullptr) {
        ALOGE("%s: Error: out null", __FUNCTION__);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    *out = new ACaptureSessionOutputContainer();
    return ACAMERA_OK;
}

EXPORT
void ACaptureSessionOutputContainer_free(ACaptureSessionOutputContainer* container) {
    ATRACE_CALL();
    if (container != nullptr) {
        delete container;
    }
    return;
}

EXPORT
camera_status_t ACaptureSessionOutput_create(
        ANativeWindow* window, /*out*/ACaptureSessionOutput** out) {
    ATRACE_CALL();
    if (window == nullptr || out == nullptr) {
        ALOGE("%s: Error: bad argument. window %p, out %p",
                __FUNCTION__, window, out);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    *out = new ACaptureSessionOutput(window);
    return ACAMERA_OK;
}

EXPORT
void ACaptureSessionOutput_free(ACaptureSessionOutput* output) {
    ATRACE_CALL();
    if (output != nullptr) {
        delete output;
    }
    return;
}

EXPORT
camera_status_t ACaptureSessionOutputContainer_add(
        ACaptureSessionOutputContainer* container, const ACaptureSessionOutput* output) {
    ATRACE_CALL();
    if (container == nullptr || output == nullptr) {
        ALOGE("%s: Error: invalid input: container %p, output %p",
                __FUNCTION__, container, output);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    auto pair = container->mOutputs.insert(*output);
    if (!pair.second) {
        ALOGW("%s: output %p already exists!", __FUNCTION__, output);
    }
    return ACAMERA_OK;
}

EXPORT
camera_status_t ACaptureSessionOutputContainer_remove(
        ACaptureSessionOutputContainer* container, const ACaptureSessionOutput* output) {
    ATRACE_CALL();
    if (container == nullptr || output == nullptr) {
        ALOGE("%s: Error: invalid input: container %p, output %p",
                __FUNCTION__, container, output);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    container->mOutputs.erase(*output);
    return ACAMERA_OK;
}

EXPORT
camera_status_t ACameraDevice_createCaptureSession(
        ACameraDevice* device,
        const ACaptureSessionOutputContainer*       outputs,
        const ACameraCaptureSession_stateCallbacks* callbacks,
        /*out*/ACameraCaptureSession** session) {
    ATRACE_CALL();
    if (device == nullptr || outputs == nullptr || callbacks == nullptr || session == nullptr) {
        ALOGE("%s: Error: invalid input: device %p, outputs %p, callbacks %p, session %p",
                __FUNCTION__, device, outputs, callbacks, session);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    return device->createCaptureSession(outputs, callbacks, session);
}
