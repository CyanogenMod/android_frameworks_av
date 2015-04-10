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
#include "DeviceDescriptor.h"
#include <utils/String8.h>
#include <system/audio.h>

namespace android {

class HwModule;

// the IOProfile class describes the capabilities of an output or input stream.
// It is currently assumed that all combination of listed parameters are supported.
// It is used by the policy manager to determine if an output or input is suitable for
// a given use case,  open/close it accordingly and connect/disconnect audio tracks
// to/from it.
class IOProfile : public AudioPort
{
public:
    IOProfile(const String8& name, audio_port_role_t role);
    virtual ~IOProfile();

    // This method is used for both output and input.
    // If parameter updatedSamplingRate is non-NULL, it is assigned the actual sample rate.
    // For input, flags is interpreted as audio_input_flags_t.
    // TODO: merge audio_output_flags_t and audio_input_flags_t.
    bool isCompatibleProfile(audio_devices_t device,
                             String8 address,
                             uint32_t samplingRate,
                             uint32_t *updatedSamplingRate,
                             audio_format_t format,
                             audio_format_t *updatedFormat,
                             audio_channel_mask_t channelMask,
                             audio_channel_mask_t *updatedChannelMask,
                             uint32_t flags) const;

    void dump(int fd);
    void log();

    DeviceVector  mSupportedDevices; // supported devices
                                     // (devices this output can be routed to)
};

}; // namespace android
