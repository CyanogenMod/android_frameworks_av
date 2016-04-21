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

#include "DeviceDescriptor.h"
#include "AudioRoute.h"
#include <hardware/audio.h>
#include <utils/RefBase.h>
#include <utils/String8.h>
#include <utils/Errors.h>
#include <utils/Vector.h>
#include <system/audio.h>
#include <cutils/config_utils.h>
#include <string>

namespace android {

class IOProfile;
class InputProfile;
class OutputProfile;

typedef Vector<sp<IOProfile> > InputProfileCollection;
typedef Vector<sp<IOProfile> > OutputProfileCollection;
typedef Vector<sp<IOProfile> > IOProfileCollection;

class HwModule : public RefBase
{
public:
    HwModule(const char *name, uint32_t halVersion = AUDIO_DEVICE_API_VERSION_MIN);
    ~HwModule();

    const char *getName() const { return mName.string(); }


    const DeviceVector &getDeclaredDevices() const { return mDeclaredDevices; }
    void setDeclaredDevices(const DeviceVector &devices);

    const InputProfileCollection &getInputProfiles() const { return mInputProfiles; }

    const OutputProfileCollection &getOutputProfiles() const { return mOutputProfiles; }

    void setProfiles(const IOProfileCollection &profiles);

    void setHalVersion(uint32_t halVersion) { mHalVersion = halVersion; }
    uint32_t getHalVersion() const { return mHalVersion; }

    sp<DeviceDescriptor> getRouteSinkDevice(const sp<AudioRoute> &route) const;
    DeviceVector getRouteSourceDevices(const sp<AudioRoute> &route) const;
    void setRoutes(const AudioRouteVector &routes);

    status_t addOutputProfile(const sp<IOProfile> &profile);
    status_t addInputProfile(const sp<IOProfile> &profile);
    status_t addProfile(const sp<IOProfile> &profile);

    status_t addOutputProfile(String8 name, const audio_config_t *config,
            audio_devices_t device, String8 address);
    status_t removeOutputProfile(String8 name);
    status_t addInputProfile(String8 name, const audio_config_t *config,
            audio_devices_t device, String8 address);
    status_t removeInputProfile(String8 name);

    audio_module_handle_t getHandle() const { return mHandle; }

    sp<AudioPort> findPortByTagName(const String8 &tagName) const
    {
        return mPorts.findByTagName(tagName);
    }

    // TODO remove from here (split serialization)
    void dump(int fd);

    const String8 mName; // base name of the audio HW module (primary, a2dp ...)
    audio_module_handle_t mHandle;
    OutputProfileCollection mOutputProfiles; // output profiles exposed by this module
    InputProfileCollection mInputProfiles;  // input profiles exposed by this module

private:
    void refreshSupportedDevices();

    uint32_t mHalVersion; // audio HAL API version
    DeviceVector mDeclaredDevices; // devices declared in audio_policy configuration file.
    AudioRouteVector mRoutes;
    AudioPortVector mPorts;
};

class HwModuleCollection : public Vector<sp<HwModule> >
{
public:
    sp<HwModule> getModuleFromName(const char *name) const;

    sp<HwModule> getModuleForDevice(audio_devices_t device) const;

    sp<DeviceDescriptor> getDeviceDescriptor(const audio_devices_t device,
                                             const char *device_address,
                                             const char *device_name,
                                             bool matchAdress = true) const;

    status_t dump(int fd) const;
};

}; // namespace android
