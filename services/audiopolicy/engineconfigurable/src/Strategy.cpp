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

#define LOG_TAG "APM::AudioPolicyEngine/Strategy"

#include "Strategy.h"

using std::string;

namespace android
{
namespace audio_policy
{

status_t Element<routing_strategy>::setIdentifier(routing_strategy identifier)
{
    if (identifier >= NUM_STRATEGIES) {
        return BAD_VALUE;
    }
    mIdentifier = identifier;
    ALOGD("%s: Strategy %s identifier 0x%X", __FUNCTION__, getName().c_str(), identifier);
    return NO_ERROR;
}

/**
 * Set the device associated to this strategy.
 * It checks if the output device is valid.
 *
 * @param[in] devices selected for the given strategy.
 *
 * @return NO_ERROR if the device is either valid or none, error code otherwise.
 */
template <>
status_t Element<routing_strategy>::set<audio_devices_t>(audio_devices_t devices)
{
    if (!audio_is_output_devices(devices) || devices == AUDIO_DEVICE_NONE) {
        ALOGE("%s: trying to set an invalid device 0x%X for strategy %s",
              __FUNCTION__, devices, getName().c_str());
        return BAD_VALUE;
    }
    ALOGD("%s: 0x%X for strategy %s", __FUNCTION__, devices, getName().c_str());
    mApplicableDevices = devices;
    return NO_ERROR;
}

template <>
audio_devices_t Element<routing_strategy>::get<audio_devices_t>() const
{
    ALOGV("%s: 0x%X for strategy %s", __FUNCTION__, mApplicableDevices, getName().c_str());
    return mApplicableDevices;
}

} // namespace audio_policy
} // namespace android

