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

#define LOG_TAG "APM::AudioPolicyEngine/Usage"

#include "Usage.h"

namespace android
{
namespace audio_policy
{

status_t Element<audio_usage_t>::setIdentifier(audio_usage_t identifier)
{
    if (identifier > AUDIO_USAGE_MAX) {
        return BAD_VALUE;
    }
    mIdentifier = identifier;
    ALOGD("%s: Usage %s has identifier 0x%X", __FUNCTION__, getName().c_str(), identifier);
    return NO_ERROR;
}

template <>
status_t Element<audio_usage_t>::set<routing_strategy>(routing_strategy strategy)
{
    if (strategy >= NUM_STRATEGIES) {
        return BAD_VALUE;
    }
    ALOGD("%s: %d for Usage %s", __FUNCTION__, strategy, getName().c_str());
    mApplicableStrategy = strategy;
    return NO_ERROR;
}

template <>
routing_strategy Element<audio_usage_t>::get<routing_strategy>() const
{
    ALOGD("%s: %d for Usage %s", __FUNCTION__, mApplicableStrategy, getName().c_str());
    return mApplicableStrategy;
}

} // namespace audio_policy
} // namespace android


