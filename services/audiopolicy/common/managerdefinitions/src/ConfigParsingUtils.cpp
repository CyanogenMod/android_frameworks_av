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

#define LOG_TAG "APM::ConfigParsingUtils"
//#define LOG_NDEBUG 0

#include "ConfigParsingUtils.h"
#include "AudioGain.h"
#include <hardware/audio.h>
#include <utils/Log.h>
#include <cutils/misc.h>

namespace android {

//static
uint32_t ConfigParsingUtils::stringToEnum(const struct StringToEnum *table,
                                              size_t size,
                                              const char *name)
{
    for (size_t i = 0; i < size; i++) {
        if (strcmp(table[i].name, name) == 0) {
            ALOGV("stringToEnum() found %s", table[i].name);
            return table[i].value;
        }
    }
    return 0;
}

//static
const char *ConfigParsingUtils::enumToString(const struct StringToEnum *table,
                                              size_t size,
                                              uint32_t value)
{
    for (size_t i = 0; i < size; i++) {
        if (table[i].value == value) {
            return table[i].name;
        }
    }
    return "";
}

//static
bool ConfigParsingUtils::stringToBool(const char *value)
{
    return ((strcasecmp("true", value) == 0) || (strcmp("1", value) == 0));
}


// --- audio_policy.conf file parsing
//static
uint32_t ConfigParsingUtils::parseOutputFlagNames(char *name)
{
    uint32_t flag = 0;

    // it is OK to cast name to non const here as we are not going to use it after
    // strtok() modifies it
    char *flagName = strtok(name, "|");
    while (flagName != NULL) {
        if (strlen(flagName) != 0) {
            flag |= ConfigParsingUtils::stringToEnum(sOutputFlagNameToEnumTable,
                               ARRAY_SIZE(sOutputFlagNameToEnumTable),
                               flagName);
        }
        flagName = strtok(NULL, "|");
    }
    //force direct flag if offload flag is set: offloading implies a direct output stream
    // and all common behaviors are driven by checking only the direct flag
    // this should normally be set appropriately in the policy configuration file
    if ((flag & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
        flag |= AUDIO_OUTPUT_FLAG_DIRECT;
    }

    return flag;
}

//static
uint32_t ConfigParsingUtils::parseInputFlagNames(char *name)
{
    uint32_t flag = 0;

    // it is OK to cast name to non const here as we are not going to use it after
    // strtok() modifies it
    char *flagName = strtok(name, "|");
    while (flagName != NULL) {
        if (strlen(flagName) != 0) {
            flag |= stringToEnum(sInputFlagNameToEnumTable,
                               ARRAY_SIZE(sInputFlagNameToEnumTable),
                               flagName);
        }
        flagName = strtok(NULL, "|");
    }
    return flag;
}

//static
audio_devices_t ConfigParsingUtils::parseDeviceNames(char *name)
{
    uint32_t device = 0;

    char *devName = strtok(name, "|");
    while (devName != NULL) {
        if (strlen(devName) != 0) {
            device |= stringToEnum(sDeviceTypeToEnumTable,
                                 ARRAY_SIZE(sDeviceTypeToEnumTable),
                                 devName);
         }
        devName = strtok(NULL, "|");
     }
    return device;
}

//static
void ConfigParsingUtils::loadHwModule(cnode *root, HwModuleCollection &hwModules,
                                      DeviceVector &availableInputDevices,
                                      DeviceVector &availableOutputDevices,
                                      sp<DeviceDescriptor> &defaultOutputDevices,
                                      bool &isSpeakerDrcEnable)
{
    status_t status = NAME_NOT_FOUND;
    cnode *node;
    sp<HwModule> module = new HwModule(root->name);

    node = config_find(root, DEVICES_TAG);
    if (node != NULL) {
        node = node->first_child;
        while (node) {
            ALOGV("loadHwModule() loading device %s", node->name);
            status_t tmpStatus = module->loadDevice(node);
            if (status == NAME_NOT_FOUND || status == NO_ERROR) {
                status = tmpStatus;
            }
            node = node->next;
        }
    }
    node = config_find(root, OUTPUTS_TAG);
    if (node != NULL) {
        node = node->first_child;
        while (node) {
            ALOGV("loadHwModule() loading output %s", node->name);
            status_t tmpStatus = module->loadOutput(node);
            if (status == NAME_NOT_FOUND || status == NO_ERROR) {
                status = tmpStatus;
            }
            node = node->next;
        }
    }
    node = config_find(root, INPUTS_TAG);
    if (node != NULL) {
        node = node->first_child;
        while (node) {
            ALOGV("loadHwModule() loading input %s", node->name);
            status_t tmpStatus = module->loadInput(node);
            if (status == NAME_NOT_FOUND || status == NO_ERROR) {
                status = tmpStatus;
            }
            node = node->next;
        }
    }
    loadGlobalConfig(root, module, availableInputDevices, availableOutputDevices,
                     defaultOutputDevices, isSpeakerDrcEnable);

    if (status == NO_ERROR) {
        hwModules.add(module);
    }
}

//static
void ConfigParsingUtils::loadHwModules(cnode *root, HwModuleCollection &hwModules,
                                       DeviceVector &availableInputDevices,
                                       DeviceVector &availableOutputDevices,
                                       sp<DeviceDescriptor> &defaultOutputDevices,
                                       bool &isSpeakerDrcEnabled)
{
    cnode *node = config_find(root, AUDIO_HW_MODULE_TAG);
    if (node == NULL) {
        return;
    }

    node = node->first_child;
    while (node) {
        ALOGV("loadHwModules() loading module %s", node->name);
        loadHwModule(node, hwModules, availableInputDevices, availableOutputDevices,
                     defaultOutputDevices, isSpeakerDrcEnabled);
        node = node->next;
    }
}

//static
void ConfigParsingUtils::loadGlobalConfig(cnode *root, const sp<HwModule>& module,
                                          DeviceVector &availableInputDevices,
                                          DeviceVector &availableOutputDevices,
                                          sp<DeviceDescriptor> &defaultOutputDevice,
                                          bool &speakerDrcEnabled)
{
    cnode *node = config_find(root, GLOBAL_CONFIG_TAG);

    if (node == NULL) {
        return;
    }
    DeviceVector declaredDevices;
    if (module != NULL) {
        declaredDevices = module->mDeclaredDevices;
    }

    node = node->first_child;
    while (node) {
        if (strcmp(ATTACHED_OUTPUT_DEVICES_TAG, node->name) == 0) {
            availableOutputDevices.loadDevicesFromTag((char *)node->value,
                                                        declaredDevices);
            ALOGV("loadGlobalConfig() Attached Output Devices %08x",
                  availableOutputDevices.types());
        } else if (strcmp(DEFAULT_OUTPUT_DEVICE_TAG, node->name) == 0) {
            audio_devices_t device = (audio_devices_t)stringToEnum(
                    sDeviceTypeToEnumTable,
                    ARRAY_SIZE(sDeviceTypeToEnumTable),
                    (char *)node->value);
            if (device != AUDIO_DEVICE_NONE) {
                defaultOutputDevice = new DeviceDescriptor(device);
            } else {
                ALOGW("loadGlobalConfig() default device not specified");
            }
            ALOGV("loadGlobalConfig() mDefaultOutputDevice %08x", defaultOutputDevice->type());
        } else if (strcmp(ATTACHED_INPUT_DEVICES_TAG, node->name) == 0) {
            availableInputDevices.loadDevicesFromTag((char *)node->value,
                                                       declaredDevices);
            ALOGV("loadGlobalConfig() Available InputDevices %08x", availableInputDevices.types());
        } else if (strcmp(SPEAKER_DRC_ENABLED_TAG, node->name) == 0) {
            speakerDrcEnabled = stringToBool((char *)node->value);
            ALOGV("loadGlobalConfig() mSpeakerDrcEnabled = %d", speakerDrcEnabled);
        } else if (strcmp(AUDIO_HAL_VERSION_TAG, node->name) == 0) {
            uint32_t major, minor;
            sscanf((char *)node->value, "%u.%u", &major, &minor);
            module->mHalVersion = HARDWARE_DEVICE_API_VERSION(major, minor);
            ALOGV("loadGlobalConfig() mHalVersion = %04x major %u minor %u",
                  module->mHalVersion, major, minor);
        }
        node = node->next;
    }
}

//static
status_t ConfigParsingUtils::loadAudioPolicyConfig(const char *path,
                                                   HwModuleCollection &hwModules,
                                                   DeviceVector &availableInputDevices,
                                                   DeviceVector &availableOutputDevices,
                                                   sp<DeviceDescriptor> &defaultOutputDevices,
                                                   bool &isSpeakerDrcEnabled)
{
    cnode *root;
    char *data;

    data = (char *)load_file(path, NULL);
    if (data == NULL) {
        return -ENODEV;
    }
    root = config_node("", "");
    config_load(root, data);

    loadHwModules(root, hwModules,
                  availableInputDevices, availableOutputDevices,
                  defaultOutputDevices, isSpeakerDrcEnabled);
    // legacy audio_policy.conf files have one global_configuration section
    loadGlobalConfig(root, hwModules.getModuleFromName(AUDIO_HARDWARE_MODULE_ID_PRIMARY),
                     availableInputDevices, availableOutputDevices,
                     defaultOutputDevices, isSpeakerDrcEnabled);
    config_free(root);
    free(root);
    free(data);

    ALOGI("loadAudioPolicyConfig() loaded %s\n", path);

    return NO_ERROR;
}

}; // namespace android
