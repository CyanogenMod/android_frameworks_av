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
#define LOG_TAG "NdkCameraMetadata"
#define ATRACE_TAG ATRACE_TAG_CAMERA

#include <utils/Log.h>
#include <utils/Trace.h>

#include "NdkCameraMetadata.h"
#include "impl/ACameraMetadata.h"

using namespace android;

EXPORT
camera_status_t ACameraMetadata_getConstEntry(
        const ACameraMetadata* acm, uint32_t tag, ACameraMetadata_const_entry* entry) {
    ATRACE_CALL();
    if (acm == nullptr || entry == nullptr) {
        ALOGE("%s: invalid argument! metadata 0x%p, tag 0x%x, entry 0x%p",
               __FUNCTION__, acm, tag, entry);
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    return acm->getConstEntry(tag, entry);
}

EXPORT
ACameraMetadata* ACameraMetadata_copy(const ACameraMetadata* src) {
    ATRACE_CALL();
    if (src == nullptr) {
        ALOGE("%s: src is null!", __FUNCTION__);
        return nullptr;
    }
    return new ACameraMetadata(*src);
}

EXPORT
void ACameraMetadata_free(ACameraMetadata* metadata) {
    ATRACE_CALL();
    if (metadata != nullptr) {
        delete metadata;
    }
}
