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

#ifndef _NDK_CAMERA_MANAGER_H
#define _NDK_CAMERA_MANAGER_H

#include "NdkCameraError.h"
#include "NdkCameraMetadata.h"
#include "NdkCameraDevice.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ACameraManager ACameraManager;

/**
 * Create CameraManager instance.
 * The caller must call ACameraManager_delete to free the resources
 */
ACameraManager* ACameraManager_create();

/**
 * delete the ACameraManager and free its resources
 */
void ACameraManager_delete(ACameraManager*);

// Struct to hold list of camera devices
typedef struct ACameraIdList {
    int numCameras;
    const char** cameraIds;
} ACameraIdList;

/**
 * Create/delete a list of camera devices.
 * ACameraManager_getCameraIdList will allocate and return an ACameraIdList.
 * The caller must call ACameraManager_deleteCameraIdList to free the memory
 */
camera_status_t ACameraManager_getCameraIdList(ACameraManager*,
                                              /*out*/ACameraIdList** cameraIdList);
void ACameraManager_deleteCameraIdList(ACameraIdList* cameraIdList);


// Struct to hold camera availability callbacks
typedef void (*ACameraManager_AvailabilityCallback)(void* context, const char* cameraId);

typedef struct ACameraManager_AvailabilityListener {
    void*                               context; // optional application context.
    ACameraManager_AvailabilityCallback onCameraAvailable;
    ACameraManager_AvailabilityCallback onCameraUnavailable;
} ACameraManager_AvailabilityCallbacks;

/**
 * register/unregister camera availability callbacks
 */
camera_status_t ACameraManager_registerAvailabilityCallback(
        ACameraManager*, const ACameraManager_AvailabilityCallbacks *callback);
camera_status_t ACameraManager_unregisterAvailabilityCallback(
        ACameraManager*, const ACameraManager_AvailabilityCallbacks *callback);

/**
 * Query the characteristics of a camera.
 * The caller must call ACameraMetadata_free to free the memory of the output characteristics.
 */
camera_status_t ACameraManager_getCameraCharacteristics(
        ACameraManager*, const char *cameraId,
        /*out*/ACameraMetadata **characteristics);

/**
 * Open a camera device synchronously.
 * The opened camera device will be returned in
 */
camera_status_t ACameraManager_openCamera(
        ACameraManager*, const char* cameraId,
        ACameraDevice_StateCallbacks* callback,
        /*out*/ACameraDevice** device);

#ifdef __cplusplus
} // extern "C"
#endif

#endif //_NDK_CAMERA_MANAGER_H
