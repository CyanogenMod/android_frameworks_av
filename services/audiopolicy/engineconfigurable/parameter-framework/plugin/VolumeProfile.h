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

#include "FormattedSubsystemObject.h"
#include "InstanceConfigurableElement.h"
#include "MappingContext.h"
#include <Volume.h>
#include <AudioPolicyPluginInterface.h>
#include <string>

class PolicySubsystem;

class VolumeProfile : public CFormattedSubsystemObject
{
private:
    struct Point
    {
        int index;
        /** Volume is using FixedPointParameter until float parameters are available. */
        int16_t dbAttenuation;
    } __attribute__((packed));

public:
    VolumeProfile(const std::string &mappingValue,
                  CInstanceConfigurableElement *instanceConfigurableElement,
                  const CMappingContext &context);

protected:
    virtual bool receiveFromHW(std::string &error);
    virtual bool sendToHW(std::string &error);

private:
    const PolicySubsystem *mPolicySubsystem; /**< Route subsytem plugin. */

    /**
     * Interface to communicate with Audio Policy Engine.
     */
    android::AudioPolicyPluginInterface *mPolicyPluginInterface;

    /**
     * volume profile identifier,  which is in fact a stream type to link with audio.h.
     */
    audio_stream_type_t mId;

    size_t mPoints;
    device_category mCategory;

    static const uint32_t gFractional = 8; /**< Beware to align with the structure. */
};
