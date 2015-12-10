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

#include <android/native_window.h>
#include "NdkCameraError.h"
#include "NdkCaptureRequest.h"
#include "NdkCameraCaptureSession.h"

#ifndef _NDK_CAMERA_DEVICE_H
#define _NDK_CAMERA_DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ACameraDevice ACameraDevice;

// Struct to hold camera state callbacks
typedef void (*ACameraDevice_StateCallback)(void* context, ACameraDevice* device);
typedef void (*ACameraDevice_ErrorStateCallback)(void* context, ACameraDevice* device, int error);

typedef struct ACameraDevice_StateCallbacks {
    void*                             context;
    ACameraDevice_StateCallback       onDisconnected;
    ACameraDevice_ErrorStateCallback  onError;
} ACameraDevice_stateCallbacks;

/**
 * Close the camera device synchronously. Open is done in ACameraManager_openCamera
 */
camera_status_t ACameraDevice_close(ACameraDevice*);

/**
 * Return the camera id associated with this camera device
 * The returned pointer is still owned by framework and should not be delete/free by app
 * The returned pointer should not be used after the device has been closed
 */
const char* ACameraDevice_getId(const ACameraDevice*);

typedef enum {
    TEMPLATE_PREVIEW = 1,
    TEMPLATE_STILL_CAPTURE,
    TEMPLATE_RECORD,
    TEMPLATE_VIDEO_SNAPSHOT,
    TEMPLATE_ZERO_SHUTTER_LAG,
    TEMPLATE_MANUAL,
} ACameraDevice_request_template;

/**
 * Create/free a default capture request for input template
 */
camera_status_t ACameraDevice_createCaptureRequest(
        const ACameraDevice*, ACameraDevice_request_template, /*out*/ACaptureRequest** request);

/**
 * APIs for createing capture session
 */
typedef struct ACaptureSessionOutputContainer ACaptureSessionOutputContainer;

typedef struct ACaptureSessionOutput ACaptureSessionOutput;

camera_status_t ACaptureSessionOutputContainer_create(/*out*/ACaptureSessionOutputContainer**);
void            ACaptureSessionOutputContainer_free(ACaptureSessionOutputContainer*);

camera_status_t ACaptureSessionOutput_create(ANativeWindow*, /*out*/ACaptureSessionOutput**);
void            ACaptureSessionOutput_free(ACaptureSessionOutput*);

camera_status_t ACaptureSessionOutputContainer_add(
        ACaptureSessionOutputContainer*, const ACaptureSessionOutput*);
camera_status_t ACaptureSessionOutputContainer_remove(
        ACaptureSessionOutputContainer*, const ACaptureSessionOutput*);

camera_status_t ACameraDevice_createCaptureSession(
        ACameraDevice*,
        const ACaptureSessionOutputContainer*       outputs,
        const ACameraCaptureSession_stateCallbacks* callbacks);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _NDK_CAMERA_DEVICE_H
