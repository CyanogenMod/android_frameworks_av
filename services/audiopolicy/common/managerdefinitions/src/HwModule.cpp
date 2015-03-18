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

#define LOG_TAG "APM::HwModule"
//#define LOG_NDEBUG 0

#include "HwModule.h"
#include "IOProfile.h"
#include "AudioGain.h"
#include "ConfigParsingUtils.h"
#include "audio_policy_conf.h"
#include <hardware/audio.h>

namespace android {

HwModule::HwModule(const char *name)
    : mName(strndup(name, AUDIO_HARDWARE_MODULE_ID_MAX_LEN)),
      mHalVersion(AUDIO_DEVICE_API_VERSION_MIN), mHandle(0)
{
}

HwModule::~HwModule()
{
    for (size_t i = 0; i < mOutputProfiles.size(); i++) {
        mOutputProfiles[i]->mSupportedDevices.clear();
    }
    for (size_t i = 0; i < mInputProfiles.size(); i++) {
        mInputProfiles[i]->mSupportedDevices.clear();
    }
    free((void *)mName);
}

status_t HwModule::loadInput(cnode *root)
{
    cnode *node = root->first_child;

    sp<IOProfile> profile = new IOProfile(String8(root->name), AUDIO_PORT_ROLE_SINK, this);

    while (node) {
        if (strcmp(node->name, SAMPLING_RATES_TAG) == 0) {
            profile->loadSamplingRates((char *)node->value);
        } else if (strcmp(node->name, FORMATS_TAG) == 0) {
            profile->loadFormats((char *)node->value);
        } else if (strcmp(node->name, CHANNELS_TAG) == 0) {
            profile->loadInChannels((char *)node->value);
        } else if (strcmp(node->name, DEVICES_TAG) == 0) {
            profile->mSupportedDevices.loadDevicesFromName((char *)node->value,
                                                           mDeclaredDevices);
        } else if (strcmp(node->name, FLAGS_TAG) == 0) {
            profile->mFlags = ConfigParsingUtils::parseInputFlagNames((char *)node->value);
        } else if (strcmp(node->name, GAINS_TAG) == 0) {
            profile->loadGains(node);
        }
        node = node->next;
    }
    ALOGW_IF(profile->mSupportedDevices.isEmpty(),
            "loadInput() invalid supported devices");
    ALOGW_IF(profile->mChannelMasks.size() == 0,
            "loadInput() invalid supported channel masks");
    ALOGW_IF(profile->mSamplingRates.size() == 0,
            "loadInput() invalid supported sampling rates");
    ALOGW_IF(profile->mFormats.size() == 0,
            "loadInput() invalid supported formats");
    if (!profile->mSupportedDevices.isEmpty() &&
            (profile->mChannelMasks.size() != 0) &&
            (profile->mSamplingRates.size() != 0) &&
            (profile->mFormats.size() != 0)) {

        ALOGV("loadInput() adding input Supported Devices %04x",
              profile->mSupportedDevices.types());

        mInputProfiles.add(profile);
        return NO_ERROR;
    } else {
        return BAD_VALUE;
    }
}

status_t HwModule::loadOutput(cnode *root)
{
    cnode *node = root->first_child;

    sp<IOProfile> profile = new IOProfile(String8(root->name), AUDIO_PORT_ROLE_SOURCE, this);

    while (node) {
        if (strcmp(node->name, SAMPLING_RATES_TAG) == 0) {
            profile->loadSamplingRates((char *)node->value);
        } else if (strcmp(node->name, FORMATS_TAG) == 0) {
            profile->loadFormats((char *)node->value);
        } else if (strcmp(node->name, CHANNELS_TAG) == 0) {
            profile->loadOutChannels((char *)node->value);
        } else if (strcmp(node->name, DEVICES_TAG) == 0) {
            profile->mSupportedDevices.loadDevicesFromName((char *)node->value,
                                                           mDeclaredDevices);
        } else if (strcmp(node->name, FLAGS_TAG) == 0) {
            profile->mFlags = ConfigParsingUtils::parseOutputFlagNames((char *)node->value);
        } else if (strcmp(node->name, GAINS_TAG) == 0) {
            profile->loadGains(node);
        }
        node = node->next;
    }
    ALOGW_IF(profile->mSupportedDevices.isEmpty(),
            "loadOutput() invalid supported devices");
    ALOGW_IF(profile->mChannelMasks.size() == 0,
            "loadOutput() invalid supported channel masks");
    ALOGW_IF(profile->mSamplingRates.size() == 0,
            "loadOutput() invalid supported sampling rates");
    ALOGW_IF(profile->mFormats.size() == 0,
            "loadOutput() invalid supported formats");
    if (!profile->mSupportedDevices.isEmpty() &&
            (profile->mChannelMasks.size() != 0) &&
            (profile->mSamplingRates.size() != 0) &&
            (profile->mFormats.size() != 0)) {

        ALOGV("loadOutput() adding output Supported Devices %04x, mFlags %04x",
              profile->mSupportedDevices.types(), profile->mFlags);

        mOutputProfiles.add(profile);
        return NO_ERROR;
    } else {
        return BAD_VALUE;
    }
}

status_t HwModule::loadDevice(cnode *root)
{
    cnode *node = root->first_child;

    audio_devices_t type = AUDIO_DEVICE_NONE;
    while (node) {
        if (strcmp(node->name, DEVICE_TYPE) == 0) {
            type = ConfigParsingUtils::parseDeviceNames((char *)node->value);
            break;
        }
        node = node->next;
    }
    if (type == AUDIO_DEVICE_NONE ||
            (!audio_is_input_device(type) && !audio_is_output_device(type))) {
        ALOGW("loadDevice() bad type %08x", type);
        return BAD_VALUE;
    }
    sp<DeviceDescriptor> deviceDesc = new DeviceDescriptor(String8(root->name), type);
    deviceDesc->mModule = this;

    node = root->first_child;
    while (node) {
        if (strcmp(node->name, DEVICE_ADDRESS) == 0) {
            deviceDesc->mAddress = String8((char *)node->value);
        } else if (strcmp(node->name, CHANNELS_TAG) == 0) {
            if (audio_is_input_device(type)) {
                deviceDesc->loadInChannels((char *)node->value);
            } else {
                deviceDesc->loadOutChannels((char *)node->value);
            }
        } else if (strcmp(node->name, GAINS_TAG) == 0) {
            deviceDesc->loadGains(node);
        }
        node = node->next;
    }

    ALOGV("loadDevice() adding device name %s type %08x address %s",
          deviceDesc->mName.string(), type, deviceDesc->mAddress.string());

    mDeclaredDevices.add(deviceDesc);

    return NO_ERROR;
}

status_t HwModule::addOutputProfile(String8 name, const audio_config_t *config,
                                                  audio_devices_t device, String8 address)
{
    sp<IOProfile> profile = new IOProfile(name, AUDIO_PORT_ROLE_SOURCE, this);

    profile->mSamplingRates.add(config->sample_rate);
    profile->mChannelMasks.add(config->channel_mask);
    profile->mFormats.add(config->format);

    sp<DeviceDescriptor> devDesc = new DeviceDescriptor(name, device);
    devDesc->mAddress = address;
    profile->mSupportedDevices.add(devDesc);

    mOutputProfiles.add(profile);

    return NO_ERROR;
}

status_t HwModule::removeOutputProfile(String8 name)
{
    for (size_t i = 0; i < mOutputProfiles.size(); i++) {
        if (mOutputProfiles[i]->mName == name) {
            mOutputProfiles.removeAt(i);
            break;
        }
    }

    return NO_ERROR;
}

status_t HwModule::addInputProfile(String8 name, const audio_config_t *config,
                                                  audio_devices_t device, String8 address)
{
    sp<IOProfile> profile = new IOProfile(name, AUDIO_PORT_ROLE_SINK, this);

    profile->mSamplingRates.add(config->sample_rate);
    profile->mChannelMasks.add(config->channel_mask);
    profile->mFormats.add(config->format);

    sp<DeviceDescriptor> devDesc = new DeviceDescriptor(name, device);
    devDesc->mAddress = address;
    profile->mSupportedDevices.add(devDesc);

    ALOGV("addInputProfile() name %s rate %d mask 0x08", name.string(), config->sample_rate, config->channel_mask);

    mInputProfiles.add(profile);

    return NO_ERROR;
}

status_t HwModule::removeInputProfile(String8 name)
{
    for (size_t i = 0; i < mInputProfiles.size(); i++) {
        if (mInputProfiles[i]->mName == name) {
            mInputProfiles.removeAt(i);
            break;
        }
    }

    return NO_ERROR;
}


void HwModule::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "  - name: %s\n", mName);
    result.append(buffer);
    snprintf(buffer, SIZE, "  - handle: %d\n", mHandle);
    result.append(buffer);
    snprintf(buffer, SIZE, "  - version: %u.%u\n", mHalVersion >> 8, mHalVersion & 0xFF);
    result.append(buffer);
    write(fd, result.string(), result.size());
    if (mOutputProfiles.size()) {
        write(fd, "  - outputs:\n", strlen("  - outputs:\n"));
        for (size_t i = 0; i < mOutputProfiles.size(); i++) {
            snprintf(buffer, SIZE, "    output %zu:\n", i);
            write(fd, buffer, strlen(buffer));
            mOutputProfiles[i]->dump(fd);
        }
    }
    if (mInputProfiles.size()) {
        write(fd, "  - inputs:\n", strlen("  - inputs:\n"));
        for (size_t i = 0; i < mInputProfiles.size(); i++) {
            snprintf(buffer, SIZE, "    input %zu:\n", i);
            write(fd, buffer, strlen(buffer));
            mInputProfiles[i]->dump(fd);
        }
    }
    if (mDeclaredDevices.size()) {
        write(fd, "  - devices:\n", strlen("  - devices:\n"));
        for (size_t i = 0; i < mDeclaredDevices.size(); i++) {
            mDeclaredDevices[i]->dump(fd, 4, i);
        }
    }
}

} //namespace android
