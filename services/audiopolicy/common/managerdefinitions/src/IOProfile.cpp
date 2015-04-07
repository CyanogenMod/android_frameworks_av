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

#define LOG_TAG "APM::IOProfile"
//#define LOG_NDEBUG 0

#include "IOProfile.h"
#include "HwModule.h"
#include "AudioGain.h"

namespace android {

IOProfile::IOProfile(const String8& name, audio_port_role_t role)
    : AudioPort(name, AUDIO_PORT_TYPE_MIX, role)
{
}

IOProfile::~IOProfile()
{
}

// checks if the IO profile is compatible with specified parameters.
// Sampling rate, format and channel mask must be specified in order to
// get a valid a match
bool IOProfile::isCompatibleProfile(audio_devices_t device,
                                    String8 address,
                                    uint32_t samplingRate,
                                    uint32_t *updatedSamplingRate,
                                    audio_format_t format,
                                    audio_format_t *updatedFormat,
                                    audio_channel_mask_t channelMask,
                                    audio_channel_mask_t *updatedChannelMask,
                                    uint32_t flags) const
{
    const bool isPlaybackThread = mType == AUDIO_PORT_TYPE_MIX && mRole == AUDIO_PORT_ROLE_SOURCE;
    const bool isRecordThread = mType == AUDIO_PORT_TYPE_MIX && mRole == AUDIO_PORT_ROLE_SINK;
    ALOG_ASSERT(isPlaybackThread != isRecordThread);


    if (device != AUDIO_DEVICE_NONE) {
        // just check types if multiple devices are selected
        if (popcount(device & ~AUDIO_DEVICE_BIT_IN) > 1) {
            if ((mSupportedDevices.types() & device) != device) {
                return false;
            }
        } else if (mSupportedDevices.getDevice(device, address) == 0) {
            return false;
        }
    }

    if (samplingRate == 0) {
         return false;
    }
    uint32_t myUpdatedSamplingRate = samplingRate;
    if (isPlaybackThread && checkExactSamplingRate(samplingRate) != NO_ERROR) {
         return false;
    }
    if (isRecordThread && checkCompatibleSamplingRate(samplingRate, &myUpdatedSamplingRate) !=
            NO_ERROR) {
         return false;
    }

    if (!audio_is_valid_format(format)) {
        return false;
    }
    if (isPlaybackThread && checkExactFormat(format) != NO_ERROR) {
        return false;
    }
    audio_format_t myUpdatedFormat = format;
    if (isRecordThread && checkCompatibleFormat(format, &myUpdatedFormat) != NO_ERROR) {
        return false;
    }

    if (isPlaybackThread && (!audio_is_output_channel(channelMask) ||
            checkExactChannelMask(channelMask) != NO_ERROR)) {
        return false;
    }
    audio_channel_mask_t myUpdatedChannelMask = channelMask;
    if (isRecordThread && (!audio_is_input_channel(channelMask) ||
            checkCompatibleChannelMask(channelMask, &myUpdatedChannelMask) != NO_ERROR)) {
        return false;
    }

    if (isPlaybackThread && (mFlags & flags) != flags) {
        return false;
    }
    // The only input flag that is allowed to be different is the fast flag.
    // An existing fast stream is compatible with a normal track request.
    // An existing normal stream is compatible with a fast track request,
    // but the fast request will be denied by AudioFlinger and converted to normal track.
    if (isRecordThread && ((mFlags ^ flags) &
            ~AUDIO_INPUT_FLAG_FAST)) {
        return false;
    }

    if (updatedSamplingRate != NULL) {
        *updatedSamplingRate = myUpdatedSamplingRate;
    }
    if (updatedFormat != NULL) {
        *updatedFormat = myUpdatedFormat;
    }
    if (updatedChannelMask != NULL) {
        *updatedChannelMask = myUpdatedChannelMask;
    }
    return true;
}

void IOProfile::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    AudioPort::dump(fd, 4);

    snprintf(buffer, SIZE, "    - flags: 0x%04x\n", mFlags);
    result.append(buffer);
    snprintf(buffer, SIZE, "    - devices:\n");
    result.append(buffer);
    write(fd, result.string(), result.size());
    for (size_t i = 0; i < mSupportedDevices.size(); i++) {
        mSupportedDevices[i]->dump(fd, 6, i);
    }
}

void IOProfile::log()
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    ALOGV("    - sampling rates: ");
    for (size_t i = 0; i < mSamplingRates.size(); i++) {
        ALOGV("  %d", mSamplingRates[i]);
    }

    ALOGV("    - channel masks: ");
    for (size_t i = 0; i < mChannelMasks.size(); i++) {
        ALOGV("  0x%04x", mChannelMasks[i]);
    }

    ALOGV("    - formats: ");
    for (size_t i = 0; i < mFormats.size(); i++) {
        ALOGV("  0x%08x", mFormats[i]);
    }

    ALOGV("    - devices: 0x%04x\n", mSupportedDevices.types());
    ALOGV("    - flags: 0x%04x\n", mFlags);
}

}; // namespace android
