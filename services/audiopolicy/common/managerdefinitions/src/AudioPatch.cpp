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

#define LOG_TAG "APM::AudioPatch"
//#define LOG_NDEBUG 0

#include "AudioPatch.h"
#include "ConfigParsingUtils.h"
#include <cutils/log.h>
#include <utils/String8.h>

namespace android {

int32_t volatile AudioPatch::mNextUniqueId = 1;

AudioPatch::AudioPatch(const struct audio_patch *patch, uid_t uid) :
    mHandle(static_cast<audio_patch_handle_t>(android_atomic_inc(&mNextUniqueId))),
    mPatch(*patch),
    mUid(uid),
    mAfPatchHandle(0)
{
}

status_t AudioPatch::dump(int fd, int spaces, int index) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "%*sAudio patch %d:\n", spaces, "", index+1);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- handle: %2d\n", spaces, "", mHandle);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- audio flinger handle: %2d\n", spaces, "", mAfPatchHandle);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- owner uid: %2d\n", spaces, "", mUid);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- %d sources:\n", spaces, "", mPatch.num_sources);
    result.append(buffer);
    for (size_t i = 0; i < mPatch.num_sources; i++) {
        if (mPatch.sources[i].type == AUDIO_PORT_TYPE_DEVICE) {
            snprintf(buffer, SIZE, "%*s- Device ID %d %s\n", spaces + 2, "",
                     mPatch.sources[i].id, ConfigParsingUtils::enumToString(sDeviceNameToEnumTable,
                                                        ARRAY_SIZE(sDeviceNameToEnumTable),
                                                        mPatch.sources[i].ext.device.type));
        } else {
            snprintf(buffer, SIZE, "%*s- Mix ID %d I/O handle %d\n", spaces + 2, "",
                     mPatch.sources[i].id, mPatch.sources[i].ext.mix.handle);
        }
        result.append(buffer);
    }
    snprintf(buffer, SIZE, "%*s- %d sinks:\n", spaces, "", mPatch.num_sinks);
    result.append(buffer);
    for (size_t i = 0; i < mPatch.num_sinks; i++) {
        if (mPatch.sinks[i].type == AUDIO_PORT_TYPE_DEVICE) {
            snprintf(buffer, SIZE, "%*s- Device ID %d %s\n", spaces + 2, "",
                     mPatch.sinks[i].id, ConfigParsingUtils::enumToString(sDeviceNameToEnumTable,
                                                        ARRAY_SIZE(sDeviceNameToEnumTable),
                                                        mPatch.sinks[i].ext.device.type));
        } else {
            snprintf(buffer, SIZE, "%*s- Mix ID %d I/O handle %d\n", spaces + 2, "",
                     mPatch.sinks[i].id, mPatch.sinks[i].ext.mix.handle);
        }
        result.append(buffer);
    }

    write(fd, result.string(), result.size());
    return NO_ERROR;
}

}; // namespace android
