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
#include "AudioGain.h"
#include "TypeConverter.h"
#include <cutils/log.h>
#include <utils/String8.h>

namespace android {

int32_t volatile AudioPatch::mNextUniqueId = 1;

AudioPatch::AudioPatch(const struct audio_patch *patch, uid_t uid) :
    mHandle(static_cast<audio_patch_handle_t>(android_atomic_inc(&mNextUniqueId))),
    mPatch(*patch),
    mUid(uid),
    mAfPatchHandle(AUDIO_PATCH_HANDLE_NONE)
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
            std::string device;
            DeviceConverter::toString(mPatch.sources[i].ext.device.type, device);
            snprintf(buffer, SIZE, "%*s- Device ID %d %s\n", spaces + 2, "",
                     mPatch.sources[i].id,
                     device.c_str());
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
            std::string device;
            DeviceConverter::toString(mPatch.sinks[i].ext.device.type, device);
            snprintf(buffer, SIZE, "%*s- Device ID %d %s\n", spaces + 2, "",
                     mPatch.sinks[i].id,
                     device.c_str());
        } else {
            snprintf(buffer, SIZE, "%*s- Mix ID %d I/O handle %d\n", spaces + 2, "",
                     mPatch.sinks[i].id, mPatch.sinks[i].ext.mix.handle);
        }
        result.append(buffer);
    }

    write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioPatchCollection::addAudioPatch(audio_patch_handle_t handle,
                                             const sp<AudioPatch>& patch)
{
    ssize_t index = indexOfKey(handle);

    if (index >= 0) {
        ALOGW("addAudioPatch() patch %d already in", handle);
        return ALREADY_EXISTS;
    }
    add(handle, patch);
    ALOGV("addAudioPatch() handle %d af handle %d num_sources %d num_sinks %d source handle %d"
            "sink handle %d",
          handle, patch->mAfPatchHandle, patch->mPatch.num_sources, patch->mPatch.num_sinks,
          patch->mPatch.sources[0].id, patch->mPatch.sinks[0].id);
    return NO_ERROR;
}

status_t AudioPatchCollection::removeAudioPatch(audio_patch_handle_t handle)
{
    ssize_t index = indexOfKey(handle);

    if (index < 0) {
        ALOGW("removeAudioPatch() patch %d not in", handle);
        return ALREADY_EXISTS;
    }
    ALOGV("removeAudioPatch() handle %d af handle %d", handle, valueAt(index)->mAfPatchHandle);
    removeItemsAt(index);
    return NO_ERROR;
}

status_t AudioPatchCollection::listAudioPatches(unsigned int *num_patches,
                                                struct audio_patch *patches) const
{
    if (num_patches == NULL || (*num_patches != 0 && patches == NULL)) {
        return BAD_VALUE;
    }
    ALOGV("listAudioPatches() num_patches %d patches %p available patches %zu",
          *num_patches, patches, size());
    if (patches == NULL) {
        *num_patches = 0;
    }

    size_t patchesWritten = 0;
    size_t patchesMax = *num_patches;
    for (size_t i = 0; i  < size() && patchesWritten < patchesMax; i++) {
        const sp<AudioPatch>  patch = valueAt(i);
        patches[patchesWritten] = patch->mPatch;
        patches[patchesWritten++].id = patch->mHandle;
        ALOGV("listAudioPatches() patch %zu num_sources %d num_sinks %d",
              i, patch->mPatch.num_sources, patch->mPatch.num_sinks);
    }
    *num_patches = size();

    ALOGV("listAudioPatches() got %zu patches needed %d", patchesWritten, *num_patches);
    return NO_ERROR;
}

status_t AudioPatchCollection::dump(int fd) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    snprintf(buffer, SIZE, "\nAudio Patches:\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < size(); i++) {
        valueAt(i)->dump(fd, 2, i);
    }
    return NO_ERROR;
}

}; // namespace android
