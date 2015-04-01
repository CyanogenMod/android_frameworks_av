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
#include <utils/RefBase.h>
#include <utils/String8.h>
#include <utils/Errors.h>
#include <utils/Vector.h>
#include <system/audio.h>
#include <cutils/config_utils.h>

namespace android {

class IOProfile;

class HwModule : public RefBase
{
public:
    HwModule(const char *name);
    ~HwModule();

    status_t loadOutput(cnode *root);
    status_t loadInput(cnode *root);
    status_t loadDevice(cnode *root);

    status_t addOutputProfile(String8 name, const audio_config_t *config,
            audio_devices_t device, String8 address);
    status_t removeOutputProfile(String8 name);
    status_t addInputProfile(String8 name, const audio_config_t *config,
            audio_devices_t device, String8 address);
    status_t removeInputProfile(String8 name);

    audio_module_handle_t getHandle() const { return mHandle; }

    void dump(int fd);

    const char *const        mName; // base name of the audio HW module (primary, a2dp ...)
    uint32_t                 mHalVersion; // audio HAL API version
    audio_module_handle_t    mHandle;
    Vector < sp<IOProfile> > mOutputProfiles; // output profiles exposed by this module
    Vector < sp<IOProfile> > mInputProfiles;  // input profiles exposed by this module
    DeviceVector             mDeclaredDevices; // devices declared in audio_policy.conf
};

class HwModuleCollection : public Vector< sp<HwModule> >
{
public:
    sp<HwModule> getModuleFromName(const char *name) const;

    sp <HwModule> getModuleForDevice(audio_devices_t device) const;

    sp<DeviceDescriptor>  getDeviceDescriptor(const audio_devices_t device,
                                              const char *device_address,
                                              const char *device_name) const;

    status_t dump(int fd) const;
};

}; // namespace android
