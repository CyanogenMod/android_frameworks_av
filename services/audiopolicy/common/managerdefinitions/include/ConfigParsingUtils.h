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

#include "AudioPolicyConfig.h"
#include "DeviceDescriptor.h"
#include "HwModule.h"
#include "audio_policy_conf.h"
#include <system/audio.h>
#include <utils/Log.h>
#include <utils/Vector.h>
#include <utils/SortedVector.h>
#include <cutils/config_utils.h>
#include <utils/RefBase.h>
#include <system/audio_policy.h>

namespace android {

// ----------------------------------------------------------------------------
// Definitions for audio_policy.conf file parsing
// ----------------------------------------------------------------------------

class ConfigParsingUtils
{
public:
    static status_t loadConfig(const char *path, AudioPolicyConfig &config);

private:
    static uint32_t parseOutputFlagNames(const char *name);
    static void loadAudioPortGain(cnode *root, AudioPort &audioPort, int index);
    static void loadAudioPortGains(cnode *root, AudioPort &audioPort);
    static void loadDeviceDescriptorGains(cnode *root, sp<DeviceDescriptor> &deviceDesc);
    static status_t loadHwModuleDevice(cnode *root, DeviceVector &devices);
    static status_t loadHwModuleInput(cnode *root, sp<HwModule> &module);
    static status_t loadHwModuleOutput(cnode *root, sp<HwModule> &module);
    static void loadDevicesFromTag(const char *tag, DeviceVector &devices,
                            const DeviceVector &declaredDevices);
    static void loadHwModules(cnode *root, HwModuleCollection &hwModules,
                              AudioPolicyConfig &config);
    static void loadGlobalConfig(cnode *root, AudioPolicyConfig &config,
                                 const sp<HwModule> &primaryModule);
    static void loadModuleGlobalConfig(cnode *root, const sp<HwModule> &module,
                                       AudioPolicyConfig &config);
    static status_t loadHwModule(cnode *root, sp<HwModule> &module, AudioPolicyConfig &config);
};

}; // namespace android
