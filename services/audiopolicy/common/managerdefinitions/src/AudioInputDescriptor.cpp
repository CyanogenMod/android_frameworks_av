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

#include "AudioInputDescriptor.h"
#include "IOProfile.h"
#include "AudioGain.h"
#include "HwModule.h"
#include <media/AudioPolicy.h>
#include <policy.h>

namespace android {

AudioInputDescriptor::AudioInputDescriptor(const sp<IOProfile>& profile)
    : mIoHandle(0),
      mDevice(AUDIO_DEVICE_NONE), mPolicyMix(NULL), mPatchHandle(0), mRefCount(0),
      mInputSource(AUDIO_SOURCE_DEFAULT), mProfile(profile), mIsSoundTrigger(false), mId(0)
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

void AudioInputDescriptor::setIoHandle(audio_io_handle_t ioHandle)
{
    mId = AudioPort::getNextUniqueId();
    mIoHandle = ioHandle;
}

audio_module_handle_t AudioInputDescriptor::getModuleHandle() const
{
    if (mProfile == 0) {
        return 0;
    }
    return mProfile->getModuleHandle();
}

audio_port_handle_t AudioInputDescriptor::getId() const
{
    return mId;
}

void AudioInputDescriptor::toAudioPortConfig(struct audio_port_config *dstConfig,
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
    dstConfig->ext.mix.hw_module = getModuleHandle();
    dstConfig->ext.mix.handle = mIoHandle;
    dstConfig->ext.mix.usecase.source = mInputSource;
}

void AudioInputDescriptor::toAudioPort(struct audio_port *port) const
{
    ALOG_ASSERT(mProfile != 0, "toAudioPort() called on input with null profile %d", mIoHandle);

    mProfile->toAudioPort(port);
    port->id = mId;
    toAudioPortConfig(&port->active_config);
    port->ext.mix.hw_module = getModuleHandle();
    port->ext.mix.handle = mIoHandle;
    port->ext.mix.latency_class = AUDIO_LATENCY_NORMAL;
}

void AudioInputDescriptor::setPreemptedSessions(const SortedVector<audio_session_t>& sessions)
{
    mPreemptedSessions = sessions;
}

SortedVector<audio_session_t> AudioInputDescriptor::getPreemptedSessions() const
{
    return mPreemptedSessions;
}

bool AudioInputDescriptor::hasPreemptedSession(audio_session_t session) const
{
    return (mPreemptedSessions.indexOf(session) >= 0);
}

void AudioInputDescriptor::clearPreemptedSessions()
{
    mPreemptedSessions.clear();
}

status_t AudioInputDescriptor::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, " ID: %d\n", getId());
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

bool AudioInputCollection::isSourceActive(audio_source_t source) const
{
    for (size_t i = 0; i < size(); i++) {
        const sp<AudioInputDescriptor>  inputDescriptor = valueAt(i);
        if (inputDescriptor->mRefCount == 0) {
            continue;
        }
        if (inputDescriptor->mInputSource == (int)source) {
            return true;
        }
    }
    return false;
}

sp<AudioInputDescriptor> AudioInputCollection::getInputFromId(audio_port_handle_t id) const
{
    sp<AudioInputDescriptor> inputDesc = NULL;
    for (size_t i = 0; i < size(); i++) {
        inputDesc = valueAt(i);
        if (inputDesc->getId() == id) {
            break;
        }
    }
    return inputDesc;
}

uint32_t AudioInputCollection::activeInputsCount() const
{
    uint32_t count = 0;
    for (size_t i = 0; i < size(); i++) {
        const sp<AudioInputDescriptor>  desc = valueAt(i);
        if (desc->mRefCount > 0) {
            count++;
        }
    }
    return count;
}

audio_io_handle_t AudioInputCollection::getActiveInput(bool ignoreVirtualInputs)
{
    for (size_t i = 0; i < size(); i++) {
        const sp<AudioInputDescriptor>  input_descriptor = valueAt(i);
        if ((input_descriptor->mRefCount > 0)
                && (!ignoreVirtualInputs || !is_virtual_input_device(input_descriptor->mDevice))) {
            return keyAt(i);
        }
    }
    return 0;
}

audio_devices_t AudioInputCollection::getSupportedDevices(audio_io_handle_t handle) const
{
    sp<AudioInputDescriptor> inputDesc = valueFor(handle);
    audio_devices_t devices = inputDesc->mProfile->getSupportedDevicesType();
    return devices;
}

status_t AudioInputCollection::dump(int fd) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];

    snprintf(buffer, SIZE, "\nInputs dump:\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < size(); i++) {
        snprintf(buffer, SIZE, "- Input %d dump:\n", keyAt(i));
        write(fd, buffer, strlen(buffer));
        valueAt(i)->dump(fd);
    }

    return NO_ERROR;
}

}; //namespace android
