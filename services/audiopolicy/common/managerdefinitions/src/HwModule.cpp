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
#include <hardware/audio.h>
#include <policy.h>

namespace android {

HwModule::HwModule(const char *name, uint32_t halVersion)
    : mName(String8(name)),
      mHandle(0),
      mHalVersion(halVersion)
{
}

HwModule::~HwModule()
{
    for (size_t i = 0; i < mOutputProfiles.size(); i++) {
        mOutputProfiles[i]->clearSupportedDevices();
    }
    for (size_t i = 0; i < mInputProfiles.size(); i++) {
        mInputProfiles[i]->clearSupportedDevices();
    }
}

status_t HwModule::addOutputProfile(String8 name, const audio_config_t *config,
                                    audio_devices_t device, String8 address)
{
    sp<IOProfile> profile = new OutputProfile(name);

    profile->addAudioProfile(new AudioProfile(config->format, config->channel_mask,
                                              config->sample_rate));

    sp<DeviceDescriptor> devDesc = new DeviceDescriptor(device);
    devDesc->mAddress = address;
    profile->addSupportedDevice(devDesc);

    return addOutputProfile(profile);
}

status_t HwModule::addOutputProfile(const sp<IOProfile> &profile)
{
    profile->attach(this);
    mOutputProfiles.add(profile);
    mPorts.add(profile);
    return NO_ERROR;
}

status_t HwModule::addInputProfile(const sp<IOProfile> &profile)
{
    profile->attach(this);
    mInputProfiles.add(profile);
    mPorts.add(profile);
    return NO_ERROR;
}

status_t HwModule::addProfile(const sp<IOProfile> &profile)
{
    switch (profile->getRole()) {
    case AUDIO_PORT_ROLE_SOURCE:
        return addOutputProfile(profile);
    case AUDIO_PORT_ROLE_SINK:
        return addInputProfile(profile);
    case AUDIO_PORT_ROLE_NONE:
        return BAD_VALUE;
    }
    return BAD_VALUE;
}

void HwModule::setProfiles(const IOProfileCollection &profiles)
{
    for (size_t i = 0; i < profiles.size(); i++) {
        addProfile(profiles[i]);
    }
}

status_t HwModule::removeOutputProfile(String8 name)
{
    for (size_t i = 0; i < mOutputProfiles.size(); i++) {
        if (mOutputProfiles[i]->getName() == name) {
            mOutputProfiles.removeAt(i);
            break;
        }
    }

    return NO_ERROR;
}

status_t HwModule::addInputProfile(String8 name, const audio_config_t *config,
                                   audio_devices_t device, String8 address)
{
    sp<IOProfile> profile = new InputProfile(name);
    profile->addAudioProfile(new AudioProfile(config->format, config->channel_mask,
                                              config->sample_rate));

    sp<DeviceDescriptor> devDesc = new DeviceDescriptor(device);
    devDesc->mAddress = address;
    profile->addSupportedDevice(devDesc);

    ALOGV("addInputProfile() name %s rate %d mask 0x%08x",
          name.string(), config->sample_rate, config->channel_mask);

    return addInputProfile(profile);
}

status_t HwModule::removeInputProfile(String8 name)
{
    for (size_t i = 0; i < mInputProfiles.size(); i++) {
        if (mInputProfiles[i]->getName() == name) {
            mInputProfiles.removeAt(i);
            break;
        }
    }

    return NO_ERROR;
}

void HwModule::setDeclaredDevices(const DeviceVector &devices)
{
    mDeclaredDevices = devices;
    for (size_t i = 0; i < devices.size(); i++) {
        mPorts.add(devices[i]);
    }
}

sp<DeviceDescriptor> HwModule::getRouteSinkDevice(const sp<AudioRoute> &route) const
{
    sp<DeviceDescriptor> sinkDevice = 0;
    if (route->getSink()->getType() == AUDIO_PORT_TYPE_DEVICE) {
        sinkDevice = mDeclaredDevices.getDeviceFromTagName(route->getSink()->getTagName());
    }
    return sinkDevice;
}

DeviceVector HwModule::getRouteSourceDevices(const sp<AudioRoute> &route) const
{
    DeviceVector sourceDevices;
    Vector <sp<AudioPort> > sources = route->getSources();
    for (size_t i = 0; i < sources.size(); i++) {
        if (sources[i]->getType() == AUDIO_PORT_TYPE_DEVICE) {
            sourceDevices.add(mDeclaredDevices.getDeviceFromTagName(sources[i]->getTagName()));
        }
    }
    return sourceDevices;
}

void HwModule::setRoutes(const AudioRouteVector &routes)
{
    mRoutes = routes;
    // Now updating the streams (aka IOProfile until now) supported devices
    refreshSupportedDevices();
}

void HwModule::refreshSupportedDevices()
{
    // Now updating the streams (aka IOProfile until now) supported devices
    for (size_t i = 0; i < mInputProfiles.size(); i++) {
        sp<IOProfile> stream = mInputProfiles[i];
        DeviceVector sourceDevices;
        const AudioRouteVector &routes = stream->getRoutes();
        for (size_t j = 0; j < routes.size(); j++) {
            sp<AudioPort> sink = routes[j]->getSink();
            if (sink == 0 || stream != sink) {
                ALOGE("%s: Invalid route attached to input stream", __FUNCTION__);
                continue;
            }
            DeviceVector sourceDevicesForRoute = getRouteSourceDevices(routes[j]);
            if (sourceDevicesForRoute.isEmpty()) {
                ALOGE("%s: invalid source devices for %s", __FUNCTION__, stream->getName().string());
                continue;
            }
            sourceDevices.add(sourceDevicesForRoute);
        }
        if (sourceDevices.isEmpty()) {
            ALOGE("%s: invalid source devices for %s", __FUNCTION__, stream->getName().string());
            continue;
        }
        stream->setSupportedDevices(sourceDevices);
    }
    for (size_t i = 0; i < mOutputProfiles.size(); i++) {
        sp<IOProfile> stream = mOutputProfiles[i];
        DeviceVector sinkDevices;
        const AudioRouteVector &routes = stream->getRoutes();
        for (size_t j = 0; j < routes.size(); j++) {
            sp<AudioPort> source = routes[j]->getSources().findByTagName(stream->getTagName());
            if (source == 0 || stream != source) {
                ALOGE("%s: Invalid route attached to output stream", __FUNCTION__);
                continue;
            }
            sp<DeviceDescriptor> sinkDevice = getRouteSinkDevice(routes[j]);
            if (sinkDevice == 0) {
                ALOGE("%s: invalid sink device for %s", __FUNCTION__, stream->getName().string());
                continue;
            }
            sinkDevices.add(sinkDevice);
        }
        stream->setSupportedDevices(sinkDevices);
    }
}

void HwModule::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "  - name: %s\n", getName());
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
    mDeclaredDevices.dump(fd, String8("Declared"),  2, true);
    mRoutes.dump(fd, 2);
}

sp <HwModule> HwModuleCollection::getModuleFromName(const char *name) const
{
    sp <HwModule> module;

    for (size_t i = 0; i < size(); i++)
    {
        if (strcmp(itemAt(i)->getName(), name) == 0) {
            return itemAt(i);
        }
    }
    return module;
}


sp <HwModule> HwModuleCollection::getModuleForDevice(audio_devices_t device) const
{
    sp <HwModule> module;

    for (size_t i = 0; i < size(); i++) {
        if (itemAt(i)->getHandle() == 0) {
            continue;
        }
        if (audio_is_output_device(device)) {
            for (size_t j = 0; j < itemAt(i)->mOutputProfiles.size(); j++)
            {
                if (itemAt(i)->mOutputProfiles[j]->supportDevice(device)) {
                    return itemAt(i);
                }
            }
        } else {
            for (size_t j = 0; j < itemAt(i)->mInputProfiles.size(); j++) {
                if (itemAt(i)->mInputProfiles[j]->supportDevice(device)) {
                    return itemAt(i);
                }
            }
        }
    }
    return module;
}

sp<DeviceDescriptor>  HwModuleCollection::getDeviceDescriptor(const audio_devices_t device,
                                                              const char *device_address,
                                                              const char *device_name) const
{
    String8 address = (device_address == NULL) ? String8("") : String8(device_address);
    // handle legacy remote submix case where the address was not always specified
    if (device_distinguishes_on_address(device) && (address.length() == 0)) {
        address = String8("0");
    }

    for (size_t i = 0; i < size(); i++) {
        const sp<HwModule> hwModule = itemAt(i);
        if (hwModule->mHandle == 0) {
            continue;
        }
        DeviceVector deviceList =
                hwModule->getDeclaredDevices().getDevicesFromTypeAddr(device, address);
        if (!deviceList.isEmpty()) {
            return deviceList.itemAt(0);
        }
        deviceList = hwModule->getDeclaredDevices().getDevicesFromType(device);
        if (!deviceList.isEmpty()) {
            deviceList.itemAt(0)->setName(String8(device_name));
            deviceList.itemAt(0)->mAddress = address;
            return deviceList.itemAt(0);
        }
    }

    sp<DeviceDescriptor> devDesc = new DeviceDescriptor(device);
    devDesc->setName(String8(device_name));
    devDesc->mAddress = address;
    return devDesc;
}

status_t HwModuleCollection::dump(int fd) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];

    snprintf(buffer, SIZE, "\nHW Modules dump:\n");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < size(); i++) {
        snprintf(buffer, SIZE, "- HW Module %zu:\n", i + 1);
        write(fd, buffer, strlen(buffer));
        itemAt(i)->dump(fd);
    }
    return NO_ERROR;
}

} //namespace android
