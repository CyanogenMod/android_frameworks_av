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

#pragma once

#include <system/audio.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/RefBase.h>
#include <RoutingStrategy.h>
#include <AudioPatch.h>

namespace android {

class SwAudioOutputDescriptor;
class HwAudioOutputDescriptor;
class DeviceDescriptor;

class AudioSourceDescriptor: public RefBase
{
public:
    AudioSourceDescriptor(const sp<DeviceDescriptor> device, const audio_attributes_t *attributes,
                          uid_t uid) :
        mDevice(device), mAttributes(*attributes), mUid(uid) {}
    virtual ~AudioSourceDescriptor() {}

    audio_patch_handle_t getHandle() const { return mPatchDesc->mHandle; }

    status_t    dump(int fd);

    const sp<DeviceDescriptor> mDevice;
    const audio_attributes_t mAttributes;
    uid_t mUid;
    sp<AudioPatch> mPatchDesc;
    wp<SwAudioOutputDescriptor> mSwOutput;
    wp<HwAudioOutputDescriptor> mHwOutput;
};

class AudioSourceCollection :
        public DefaultKeyedVector< audio_patch_handle_t, sp<AudioSourceDescriptor> >
{
public:
    status_t dump(int fd) const;
};

}; // namespace android
