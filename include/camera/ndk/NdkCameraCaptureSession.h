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
    ACameraCaptureSession_stateCallback onClosed; // session is unusable after this callback
    ACameraCaptureSession_stateCallback onReady;
    ACameraCaptureSession_stateCallback onActive;
} ACameraCaptureSession_stateCallbacks;

enum {
    CAPTURE_FAILURE_REASON_FLUSHED = 0,
    CAPTURE_FAILURE_REASON_ERROR
};

typedef struct ACameraCaptureFailure {
    int64_t frameNumber;
    int     reason;
    int     sequenceId;
    bool    wasImageCaptured;
} ACameraCaptureFailure;

/* Note that the ACaptureRequest* in the callback will be different to what app has submitted,
   but the contents will still be the same as what app submitted */
typedef void (*ACameraCaptureSession_captureCallback_start)(
        void* context, ACameraCaptureSession* session,
        const ACaptureRequest* request, int64_t timestamp);

typedef void (*ACameraCaptureSession_captureCallback_result)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, const ACameraMetadata* result);

typedef void (*ACameraCaptureSession_captureCallback_failed)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, ACameraCaptureFailure* failure);

typedef void (*ACameraCaptureSession_captureCallback_sequenceEnd)(
        void* context, ACameraCaptureSession* session,
        int sequenceId, int64_t frameNumber);

typedef void (*ACameraCaptureSession_captureCallback_sequenceAbort)(
        void* context, ACameraCaptureSession* session,
        int sequenceId);

typedef struct ACameraCaptureSession_captureCallbacks {
    void*                                             context;
    ACameraCaptureSession_captureCallback_start         onCaptureStarted;
    ACameraCaptureSession_captureCallback_result        onCaptureProgressed;
    ACameraCaptureSession_captureCallback_result        onCaptureCompleted;
    ACameraCaptureSession_captureCallback_failed        onCaptureFailed;
    ACameraCaptureSession_captureCallback_sequenceEnd   onCaptureSequenceCompleted;
    ACameraCaptureSession_captureCallback_sequenceAbort onCaptureSequenceAborted;
} ACameraCaptureSession_captureCallbacks;

enum {
    CAPTURE_SEQUENCE_ID_NONE = -1
};

/*
 * Close capture session
 */
void ACameraCaptureSession_close(ACameraCaptureSession*);

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
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId);

/**
 * Send repeating capture request(s)
 */
camera_status_t ACameraCaptureSession_setRepeatingRequest(
        ACameraCaptureSession*, /*optional*/ACameraCaptureSession_captureCallbacks*,
        int numRequests, ACaptureRequest** requests,
        /*optional*/int* captureSequenceId);

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
