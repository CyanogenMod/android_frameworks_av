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

/*
 * This file defines an NDK API.
 * Do not remove methods.
 * Do not change method signatures.
 * Do not change the value of constants.
 * Do not change the size of any of the classes defined in here.
 * Do not reference types that are not part of the NDK.
 * Do not #include files that aren't part of the NDK.
 */
#include "NdkCameraError.h"
#include "NdkCameraMetadata.h"

#ifndef _NDK_CAMERA_CAPTURE_SESSION_H
#define _NDK_CAMERA_CAPTURE_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ACameraCaptureSession ACameraCaptureSession;

typedef void (*ACameraCaptureSession_stateCallback)(void* context, ACameraCaptureSession *session);

typedef struct ACameraCaptureSession_stateCallbacks {
    void*                               context;
    ACameraCaptureSession_stateCallback onConfigured;
    ACameraCaptureSession_stateCallback onConfigureFailed;
    ACameraCaptureSession_stateCallback onClosed;
    ACameraCaptureSession_stateCallback onReady;
    ACameraCaptureSession_stateCallback onActive;
} ACameraCaptureSession_stateCallbacks;

typedef struct ACameraCaptureFailure {
    uint32_t frameNumber;
    int      reason;
    int      sequenceId;
    int      wasImageCaptured;
} ACameraCaptureFailure;

typedef void (*ACameraCaptureSession_captureCallback_start)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, long timestamp);

typedef void (*ACameraCaptureSession_captureCallback_result)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, ACameraMetadata* result);

typedef void (*ACameraCaptureSession_captureCallback_failed)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, ACameraCaptureFailure* failure);

typedef void (*ACameraCaptureSession_captureCallback_sequenceEnd)(
        void* context, ACameraCaptureSession* session,
        int sequenceId, long frameNumber);

typedef struct ACameraCaptureSession_captureCallbacks {
    void*                                             context;
    ACameraCaptureSession_captureCallback_start       onCaptureStarted;
    ACameraCaptureSession_captureCallback_result      onCaptureProgressed;
    ACameraCaptureSession_captureCallback_result      onCaptureCompleted;
    ACameraCaptureSession_captureCallback_failed      onCaptureFailed;
    ACameraCaptureSession_captureCallback_sequenceEnd onCaptureSequenceCompleted;
    ACameraCaptureSession_captureCallback_sequenceEnd onCaptureSequenceAborted;
} ACameraCaptureSession_captureCallbacks;

/*
 * Close capture session
 */
camera_status_t ACameraCaptureSession_close(ACameraCaptureSession*);

struct ACameraDevice;
typedef struct ACameraDevice ACameraDevice;

/**
 * Get the camera device associated with this capture session
 */
camera_status_t ACameraCaptureSession_getDevice(
        ACameraCaptureSession*, ACameraDevice** device);

/**
 * Send capture request(s)
 */
camera_status_t ACameraCaptureSession_capture(
        ACameraCaptureSession*, /*optional*/ACameraCaptureSession_captureCallbacks*,
        int numRequests, const ACaptureRequest* requests);

/**
 * Send repeating capture request(s)
 */
camera_status_t ACameraCaptureSession_setRepeatingRequest(
        ACameraCaptureSession*, /*optional*/ACameraCaptureSession_captureCallbacks*,
        int numRequests, const ACaptureRequest* requests);

/**
 * Stop repeating capture request(s)
 */
camera_status_t ACameraCaptureSession_stopRepeating(ACameraCaptureSession*);

/**
 * Stop all capture requests as soon as possible
 */
camera_status_t ACameraCaptureSession_abortCaptures(ACameraCaptureSession*);


#ifdef __cplusplus
} // extern "C"
#endif

#endif // _NDK_CAMERA_CAPTURE_SESSION_H
