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

#include "AudioPolicyManager.h"

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
            device |= stringToEnum(sDeviceNameToEnumTable,
                                 ARRAY_SIZE(sDeviceNameToEnumTable),
                                 devName);
         }
        devName = strtok(NULL, "|");
     }
    return device;
}

}; // namespace android
