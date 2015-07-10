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

#include <system/audio.h>

// For mixed output and inputs, the policy will use max mixer sampling rates.
// Do not limit sampling rate otherwise
#define MAX_MIXER_SAMPLING_RATE 192000

// For mixed output and inputs, the policy will use max mixer channel count.
// Do not limit channel count otherwise
#define MAX_MIXER_CHANNEL_COUNT 8

/**
 * A device mask for all audio input devices that are considered "virtual" when evaluating
 * active inputs in getActiveInput()
 */
#define APM_AUDIO_IN_DEVICE_VIRTUAL_ALL  (AUDIO_DEVICE_IN_REMOTE_SUBMIX|AUDIO_DEVICE_IN_FM_TUNER)


/**
 * A device mask for all audio input and output devices where matching inputs/outputs on device
 * type alone is not enough: the address must match too
 */
#define APM_AUDIO_DEVICE_OUT_MATCH_ADDRESS_ALL (AUDIO_DEVICE_OUT_REMOTE_SUBMIX)

#define APM_AUDIO_DEVICE_IN_MATCH_ADDRESS_ALL (AUDIO_DEVICE_IN_REMOTE_SUBMIX)

/**
 * Check if the state given correspond to an in call state.
 * @TODO find a better name for widely call state
 *
 * @param[in] state to consider
 *
 * @return true if given state represents a device in a telephony or VoIP call
 */
static inline bool is_state_in_call(int state)
{
    return (state == AUDIO_MODE_IN_CALL) || (state == AUDIO_MODE_IN_COMMUNICATION);
}

/**
 * Check if the input device given is considered as a virtual device.
 *
 * @param[in] device to consider
 *
 * @return true if the device is a virtual one, false otherwise.
 */
static inline bool is_virtual_input_device(audio_devices_t device)
{
    if ((device & AUDIO_DEVICE_BIT_IN) != 0) {
        device &= ~AUDIO_DEVICE_BIT_IN;
        if ((popcount(device) == 1) && ((device & ~APM_AUDIO_IN_DEVICE_VIRTUAL_ALL) == 0))
            return true;
    }
    return false;
}

/**
 * Check whether the device type is one
 * where addresses are used to distinguish between one connected device and another
 *
 * @param[in] device to consider
 *
 * @return true if the device needs distinguish on address, false otherwise..
 */
static inline bool device_distinguishes_on_address(audio_devices_t device)
{
    return (((device & AUDIO_DEVICE_BIT_IN) != 0) &&
            ((~AUDIO_DEVICE_BIT_IN & device & APM_AUDIO_DEVICE_IN_MATCH_ADDRESS_ALL) != 0)) ||
           (((device & AUDIO_DEVICE_BIT_IN) == 0) &&
            ((device & APM_AUDIO_DEVICE_OUT_MATCH_ADDRESS_ALL) != 0));
}
