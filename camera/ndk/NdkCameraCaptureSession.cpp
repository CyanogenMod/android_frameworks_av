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
#define LOG_TAG "NdkCameraCaptureSession"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include <utils/Log.h>
#include <utils/Mutex.h>
#include <utils/StrongPointer.h>
#include <utils/Trace.h>

#include "NdkCameraDevice.h"

using namespace android;

EXPORT
camera_status_t ACameraCaptureSession_close(ACameraCaptureSession*) {
    ATRACE_CALL();
    return ACAMERA_OK;
}

EXPORT
camera_status_t ACameraCaptureSession_getDevice(
        ACameraCaptureSession*, ACameraDevice **device) {
    ATRACE_CALL();
    // Make sure don't do return garbage if device has been closed
    return ACAMERA_OK;
}

EXPORT
camera_status_t ACameraCaptureSession_capture(
        ACameraCaptureSession*, /*optional*/ACameraCaptureSession_captureCallbacks*,
        int numRequests, const ACaptureRequest* requests) {
    ATRACE_CALL();
    return ACAMERA_OK;
}

EXPORT
camera_status_t ACameraCaptureSession_setRepeatingRequest(
        ACameraCaptureSession*, /*optional*/ACameraCaptureSession_captureCallbacks*,
        int numRequests, const ACaptureRequest* requests) {
    ATRACE_CALL();
    return ACAMERA_OK;
}

EXPORT
camera_status_t ACameraCaptureSession_stopRepeating(ACameraCaptureSession*) {
    ATRACE_CALL();
    return ACAMERA_OK;
}

EXPORT
camera_status_t ACameraCaptureSession_abortCaptures(ACameraCaptureSession*) {
    ATRACE_CALL();
    return ACAMERA_OK;
}
