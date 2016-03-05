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

#include <utils/RefBase.h>
#include <media/AudioPolicy.h>
#include <utils/KeyedVector.h>
#include <hardware/audio.h>
#include <utils/String8.h>

namespace android {

class SwAudioOutputDescriptor;

/**
 * custom mix entry in mPolicyMixes
 */
class AudioPolicyMix : public RefBase {
public:
    AudioPolicyMix() {}

    const sp<SwAudioOutputDescriptor> &getOutput() const;

    void setOutput(sp<SwAudioOutputDescriptor> &output);

    void clearOutput();

    android::AudioMix *getMix();

    void setMix(AudioMix &mix);

private:
    AudioMix    mMix;                   // Audio policy mix descriptor
    sp<SwAudioOutputDescriptor> mOutput;  // Corresponding output stream
};


class AudioPolicyMixCollection : public DefaultKeyedVector<String8, sp<AudioPolicyMix> >
{
public:
    status_t getAudioPolicyMix(String8 address, sp<AudioPolicyMix> &policyMix) const;

    status_t registerMix(String8 address, AudioMix mix, sp<SwAudioOutputDescriptor> desc);

    status_t unregisterMix(String8 address);

    void closeOutput(sp<SwAudioOutputDescriptor> &desc);

    /**
     * Try to find an output descriptor for the given attributes.
     *
     * @param[in] attributes to consider fowr the research of output descriptor.
     * @param[out] desc to return if an output could be found.
     *
     * @return NO_ERROR if an output was found for the given attribute (in this case, the
     *                  descriptor output param is initialized), error code otherwise.
     */
    status_t getOutputForAttr(audio_attributes_t attributes, uid_t uid,
            sp<SwAudioOutputDescriptor> &desc);

    audio_devices_t getDeviceAndMixForInputSource(audio_source_t inputSource,
                                                  audio_devices_t availableDeviceTypes,
                                                  AudioMix **policyMix);

    status_t getInputMixForAttr(audio_attributes_t attr, AudioMix **policyMix);
};

}; // namespace android
