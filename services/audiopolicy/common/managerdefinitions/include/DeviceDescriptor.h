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

#include "AudioPort.h"
#include <utils/Errors.h>
#include <utils/String8.h>
#include <utils/SortedVector.h>
#include <cutils/config_utils.h>
#include <system/audio.h>
#include <system/audio_policy.h>

namespace android {

class DeviceDescriptor : public AudioPort, public AudioPortConfig
{
public:
    DeviceDescriptor(const String8& name, audio_devices_t type);

    virtual ~DeviceDescriptor() {}

    bool equals(const sp<DeviceDescriptor>& other) const;

    // AudioPortConfig
    virtual sp<AudioPort> getAudioPort() const { return (AudioPort*) this; }
    virtual void toAudioPortConfig(struct audio_port_config *dstConfig,
            const struct audio_port_config *srcConfig = NULL) const;

    // AudioPort
    virtual void loadGains(cnode *root);
    virtual void toAudioPort(struct audio_port *port) const;

    audio_devices_t type() const { return mDeviceType; }
    status_t dump(int fd, int spaces, int index) const;

    String8 mAddress;
    audio_port_handle_t mId;

    static String8  emptyNameStr;

private:
    audio_devices_t mDeviceType;
};

class DeviceVector : public SortedVector< sp<DeviceDescriptor> >
{
public:
    DeviceVector() : SortedVector(), mDeviceTypes(AUDIO_DEVICE_NONE) {}

    ssize_t add(const sp<DeviceDescriptor>& item);
    ssize_t remove(const sp<DeviceDescriptor>& item);
    ssize_t indexOf(const sp<DeviceDescriptor>& item) const;

    audio_devices_t types() const { return mDeviceTypes; }

    void loadDevicesFromType(audio_devices_t types);
    void loadDevicesFromName(char *name, const DeviceVector& declaredDevices);

    sp<DeviceDescriptor> getDevice(audio_devices_t type, String8 address) const;
    DeviceVector getDevicesFromType(audio_devices_t types) const;
    sp<DeviceDescriptor> getDeviceFromId(audio_port_handle_t id) const;
    sp<DeviceDescriptor> getDeviceFromName(const String8& name) const;
    DeviceVector getDevicesFromTypeAddr(audio_devices_t type, String8 address) const;

    audio_devices_t getDevicesFromHwModule(audio_module_handle_t moduleHandle) const;

    audio_policy_dev_state_t getDeviceConnectionState(const sp<DeviceDescriptor> &devDesc) const;

    status_t dump(int fd, const String8 &direction) const;

private:
    void refreshTypes();
    audio_devices_t mDeviceTypes;
};

}; // namespace android
