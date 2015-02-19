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

#define LOG_TAG "APM::AudioInputDescriptor"
//#define LOG_NDEBUG 0

#include "AudioPolicyManager.h"

namespace android {

AudioInputDescriptor::AudioInputDescriptor(const sp<IOProfile>& profile)
    : mId(0), mIoHandle(0),
      mDevice(AUDIO_DEVICE_NONE), mPolicyMix(NULL), mPatchHandle(0), mRefCount(0),
      mInputSource(AUDIO_SOURCE_DEFAULT), mProfile(profile), mIsSoundTrigger(false)
{
    if (profile != NULL) {
        mSamplingRate = profile->pickSamplingRate();
        mFormat = profile->pickFormat();
        mChannelMask = profile->pickChannelMask();
        if (profile->mGains.size() > 0) {
            profile->mGains[0]->getDefaultConfig(&mGain);
        }
    }
}

void AudioInputDescriptor::toAudioPortConfig(
                                                   struct audio_port_config *dstConfig,
                                                   const struct audio_port_config *srcConfig) const
{
    ALOG_ASSERT(mProfile != 0,
                "toAudioPortConfig() called on input with null profile %d", mIoHandle);
    dstConfig->config_mask = AUDIO_PORT_CONFIG_SAMPLE_RATE|AUDIO_PORT_CONFIG_CHANNEL_MASK|
                            AUDIO_PORT_CONFIG_FORMAT|AUDIO_PORT_CONFIG_GAIN;
    if (srcConfig != NULL) {
        dstConfig->config_mask |= srcConfig->config_mask;
    }

    AudioPortConfig::toAudioPortConfig(dstConfig, srcConfig);

    dstConfig->id = mId;
    dstConfig->role = AUDIO_PORT_ROLE_SINK;
    dstConfig->type = AUDIO_PORT_TYPE_MIX;
    dstConfig->ext.mix.hw_module = mProfile->mModule->mHandle;
    dstConfig->ext.mix.handle = mIoHandle;
    dstConfig->ext.mix.usecase.source = mInputSource;
}

void AudioInputDescriptor::toAudioPort(
                                                    struct audio_port *port) const
{
    ALOG_ASSERT(mProfile != 0, "toAudioPort() called on input with null profile %d", mIoHandle);

    mProfile->toAudioPort(port);
    port->id = mId;
    toAudioPortConfig(&port->active_config);
    port->ext.mix.hw_module = mProfile->mModule->mHandle;
    port->ext.mix.handle = mIoHandle;
    port->ext.mix.latency_class = AUDIO_LATENCY_NORMAL;
}

status_t AudioInputDescriptor::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, " ID: %d\n", mId);
    result.append(buffer);
    snprintf(buffer, SIZE, " Sampling rate: %d\n", mSamplingRate);
    result.append(buffer);
    snprintf(buffer, SIZE, " Format: %d\n", mFormat);
    result.append(buffer);
    snprintf(buffer, SIZE, " Channels: %08x\n", mChannelMask);
    result.append(buffer);
    snprintf(buffer, SIZE, " Devices %08x\n", mDevice);
    result.append(buffer);
    snprintf(buffer, SIZE, " Ref Count %d\n", mRefCount);
    result.append(buffer);
    snprintf(buffer, SIZE, " Open Ref Count %d\n", mOpenRefCount);
    result.append(buffer);

    write(fd, result.string(), result.size());

    return NO_ERROR;
}

}; //namespace android
