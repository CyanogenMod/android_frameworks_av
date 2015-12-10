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
#include "NdkCameraMetadata.h"

#ifndef _NDK_CAPTURE_REQUEST_H
#define _NDK_CAPTURE_REQUEST_H

#ifdef __cplusplus
extern "C" {
#endif

// Container for output targets
typedef struct ACameraOutputTargets ACameraOutputTargets;

// Container for a single output target
typedef struct ACameraOutputTarget ACameraOutputTarget;

typedef struct ACaptureRequest ACaptureRequest;

camera_status_t ACameraOutputTarget_create(ANativeWindow*);
void ACameraOutputTarget_free(ACameraOutputTarget*);

camera_status_t ACaptureRequest_addTarget(ACaptureRequest*, const ACameraOutputTarget*);
camera_status_t ACaptureRequest_removeTarget(ACaptureRequest*, const ACameraOutputTarget*);

/*
 * Get a metadata entry
 */
camera_status_t ACaptureRequest_getConstEntry(
        const ACaptureRequest*, uint32_t tag, ACameraMetadata_const_entry* entry);
/*
 * Set an entry of corresponding type.
 * The entry tag's type must match corresponding set API or an
 * ACAMERA_ERROR_INVALID_PARAMETER error will occur.
 * Also, the input ACameraMetadata* must belong to a capture request or an
 * ACAMERA_ERROR_INVALID_PARAMETER error will occur.
 */
camera_status_t ACaptureRequest_setEntry_u8(
        ACaptureRequest*, uint32_t tag, uint32_t count, const uint8_t* data);
camera_status_t ACaptureRequest_setEntry_i32(
        ACaptureRequest*, uint32_t tag, uint32_t count, const int32_t* data);
camera_status_t ACaptureRequest_setEntry_float(
        ACaptureRequest*, uint32_t tag, uint32_t count, const float* data);
camera_status_t ACaptureRequest_setEntry_i64(
        ACaptureRequest*, uint32_t tag, uint32_t count, const int64_t* data);
camera_status_t ACaptureRequest_setEntry_double(
        ACaptureRequest*, uint32_t tag, uint32_t count, const double* data);
camera_status_t ACaptureRequest_setEntry_rational(
        ACaptureRequest*, uint32_t tag, uint32_t count, const ACameraMetadata_rational* data);

// free the capture request created by ACameraDevice_createCaptureRequest
void ACaptureRequest_free(ACaptureRequest* request);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _NDK_CAPTURE_REQUEST_H
