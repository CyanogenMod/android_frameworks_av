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

#define LOG_TAG "APM::AudioPolicyMix"
//#define LOG_NDEBUG 0

#include "AudioPolicyMix.h"
#include "HwModule.h"
#include "AudioPort.h"
#include "IOProfile.h"
#include "AudioGain.h"
#include <AudioOutputDescriptor.h>

namespace android {

void AudioPolicyMix::setOutput(sp<SwAudioOutputDescriptor> &output)
{
    mOutput = output;
}

const sp<SwAudioOutputDescriptor> &AudioPolicyMix::getOutput() const
{
    return mOutput;
}

void AudioPolicyMix::clearOutput()
{
    mOutput.clear();
}

void AudioPolicyMix::setMix(AudioMix &mix)
{
    mMix = mix;
}

android::AudioMix *AudioPolicyMix::getMix()
{
    return &mMix;
}

status_t AudioPolicyMixCollection::registerMix(String8 address, AudioMix mix)
{
    ssize_t index = indexOfKey(address);
    if (index >= 0) {
        ALOGE("registerPolicyMixes(): mix for address %s already registered", address.string());
        return BAD_VALUE;
    }
    sp<AudioPolicyMix> policyMix = new AudioPolicyMix();
    policyMix->setMix(mix);
    add(address, policyMix);
    return NO_ERROR;
}

status_t AudioPolicyMixCollection::unregisterMix(String8 address)
{
    ssize_t index = indexOfKey(address);
    if (index < 0) {
        ALOGE("unregisterPolicyMixes(): mix for address %s not registered", address.string());
        return BAD_VALUE;
    }

    removeItemsAt(index);
    return NO_ERROR;
}

status_t AudioPolicyMixCollection::getAudioPolicyMix(String8 address,
                                                     sp<AudioPolicyMix> &policyMix) const
{
    ssize_t index = indexOfKey(address);
    if (index < 0) {
        ALOGE("unregisterPolicyMixes(): mix for address %s not registered", address.string());
        return BAD_VALUE;
    }
    policyMix = valueAt(index);
    return NO_ERROR;
}

void AudioPolicyMixCollection::closeOutput(sp<SwAudioOutputDescriptor> &desc)
{
    for (size_t i = 0; i < size(); i++) {
        sp<AudioPolicyMix> policyMix = valueAt(i);
        if (policyMix->getOutput() == desc) {
            policyMix->clearOutput();
        }
    }
}

status_t AudioPolicyMixCollection::getOutputForAttr(audio_attributes_t attributes,
                                                    sp<SwAudioOutputDescriptor> &desc)
{
    for (size_t i = 0; i < size(); i++) {
        sp<AudioPolicyMix> policyMix = valueAt(i);
        AudioMix *mix = policyMix->getMix();

        if (mix->mMixType == MIX_TYPE_PLAYERS) {
            for (size_t j = 0; j < mix->mCriteria.size(); j++) {
                if ((RULE_MATCH_ATTRIBUTE_USAGE == mix->mCriteria[j].mRule &&
                     mix->mCriteria[j].mAttr.mUsage == attributes.usage) ||
                        (RULE_EXCLUDE_ATTRIBUTE_USAGE == mix->mCriteria[j].mRule &&
                         mix->mCriteria[j].mAttr.mUsage != attributes.usage)) {
                    desc = policyMix->getOutput();
                    break;
                }
                if (strncmp(attributes.tags, "addr=", strlen("addr=")) == 0 &&
                        strncmp(attributes.tags + strlen("addr="),
                                mix->mRegistrationId.string(),
                                AUDIO_ATTRIBUTES_TAGS_MAX_SIZE - strlen("addr=") - 1) == 0) {
                    desc = policyMix->getOutput();
                    break;
                }
            }
        } else if (mix->mMixType == MIX_TYPE_RECORDERS) {
            if (attributes.usage == AUDIO_USAGE_VIRTUAL_SOURCE &&
                    strncmp(attributes.tags, "addr=", strlen("addr=")) == 0 &&
                    strncmp(attributes.tags + strlen("addr="),
                            mix->mRegistrationId.string(),
                            AUDIO_ATTRIBUTES_TAGS_MAX_SIZE - strlen("addr=") - 1) == 0) {
                desc = policyMix->getOutput();
            }
        }
        if (desc != 0) {
            desc->mPolicyMix = mix;
            return NO_ERROR;
        }
    }
    return BAD_VALUE;
}

audio_devices_t AudioPolicyMixCollection::getDeviceAndMixForInputSource(audio_source_t inputSource,
                                                                        audio_devices_t availDevices,
                                                                        AudioMix **policyMix)
{
    for (size_t i = 0; i < size(); i++) {
        AudioMix *mix = valueAt(i)->getMix();

        if (mix->mMixType != MIX_TYPE_RECORDERS) {
            continue;
        }
        for (size_t j = 0; j < mix->mCriteria.size(); j++) {
            if ((RULE_MATCH_ATTRIBUTE_CAPTURE_PRESET == mix->mCriteria[j].mRule &&
                    mix->mCriteria[j].mAttr.mSource == inputSource) ||
               (RULE_EXCLUDE_ATTRIBUTE_CAPTURE_PRESET == mix->mCriteria[j].mRule &&
                    mix->mCriteria[j].mAttr.mSource != inputSource)) {
                if (availDevices & AUDIO_DEVICE_IN_REMOTE_SUBMIX) {
                    if (policyMix != NULL) {
                        *policyMix = mix;
                    }
                    return AUDIO_DEVICE_IN_REMOTE_SUBMIX;
                }
                break;
            }
        }
    }
    return AUDIO_DEVICE_NONE;
}

status_t AudioPolicyMixCollection::getInputMixForAttr(audio_attributes_t attr, AudioMix **policyMix)
{
    if (strncmp(attr.tags, "addr=", strlen("addr=")) != 0) {
        return BAD_VALUE;
    }
    String8 address(attr.tags + strlen("addr="));

    ssize_t index = indexOfKey(address);
    if (index < 0) {
        ALOGW("getInputMixForAttr() no policy for address %s", address.string());
        return BAD_VALUE;
    }
    sp<AudioPolicyMix> audioPolicyMix = valueAt(index);
    AudioMix *mix = audioPolicyMix->getMix();

    if (mix->mMixType != MIX_TYPE_PLAYERS) {
        ALOGW("getInputMixForAttr() bad policy mix type for address %s", address.string());
        return BAD_VALUE;
    }
    *policyMix = mix;
    return NO_ERROR;
}

}; //namespace android
