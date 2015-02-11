/*
**
** Copyright 2015, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "AudioFlinger"
//#define LOG_NDEBUG 0
#include <hardware/audio.h>
#include <utils/Log.h>

#include <audio_utils/spdif/SPDIFEncoder.h>

#include "AudioHwDevice.h"
#include "AudioStreamOut.h"
#include "SpdifStreamOut.h"

namespace android {

/**
 * If the AudioFlinger is processing encoded data and the HAL expects
 * PCM then we need to wrap the data in an SPDIF wrapper.
 */
SpdifStreamOut::SpdifStreamOut(AudioHwDevice *dev, audio_output_flags_t flags)
        : AudioStreamOut(dev,flags)
        , mRateMultiplier(1)
        , mSpdifEncoder(this)
        , mRenderPositionHal(0)
        , mPreviousHalPosition32(0)
{
}

status_t SpdifStreamOut::open(
                              audio_io_handle_t handle,
                              audio_devices_t devices,
                              struct audio_config *config,
                              const char *address)
{
    struct audio_config customConfig = *config;

    customConfig.format = AUDIO_FORMAT_PCM_16_BIT;
    customConfig.channel_mask = AUDIO_CHANNEL_OUT_STEREO;

    // Some data bursts run at a higher sample rate.
    switch(config->format) {
        case AUDIO_FORMAT_E_AC3:
            mRateMultiplier = 4;
            break;
        case AUDIO_FORMAT_AC3:
            mRateMultiplier = 1;
            break;
        default:
            ALOGE("ERROR SpdifStreamOut::open() unrecognized format 0x%08X\n",
                config->format);
            return BAD_VALUE;
    }
    customConfig.sample_rate = config->sample_rate * mRateMultiplier;

    // Always print this because otherwise it could be very confusing if the
    // HAL and AudioFlinger are using different formats.
    // Print before open() because HAL may modify customConfig.
    ALOGI("SpdifStreamOut::open() AudioFlinger requested"
            " sampleRate %d, format %#x, channelMask %#x",
            config->sample_rate,
            config->format,
            config->channel_mask);
    ALOGI("SpdifStreamOut::open() HAL configured for"
            " sampleRate %d, format %#x, channelMask %#x",
            customConfig.sample_rate,
            customConfig.format,
            customConfig.channel_mask);

    status_t status = AudioStreamOut::open(
            handle,
            devices,
            &customConfig,
            address);

    ALOGI("SpdifStreamOut::open() status = %d", status);

    return status;
}

// Account for possibly higher sample rate.
status_t SpdifStreamOut::getRenderPosition(uint32_t *frames)
{
    uint32_t halPosition = 0;
    status_t status = AudioStreamOut::getRenderPosition(&halPosition);
    if (status != NO_ERROR) {
        return status;
    }

    // Accumulate a 64-bit position so that we wrap at the right place.
    if (mRateMultiplier != 1) {
        // Maintain a 64-bit render position.
        int32_t deltaHalPosition = (int32_t)(halPosition - mPreviousHalPosition32);
        mPreviousHalPosition32 = halPosition;
        mRenderPositionHal += deltaHalPosition;

        // Scale from device sample rate to application rate.
        uint64_t renderPositionApp = mRenderPositionHal / mRateMultiplier;
        ALOGV("SpdifStreamOut::getRenderPosition() "
            "renderPositionAppRate = %llu = %llu / %u\n",
            renderPositionApp, mRenderPositionHal, mRateMultiplier);

        *frames = (uint32_t)renderPositionApp;
    } else {
        *frames = halPosition;
    }
    return status;
}

int SpdifStreamOut::flush()
{
    // FIXME Is there an issue here with flush being asynchronous?
    mRenderPositionHal = 0;
    mPreviousHalPosition32 = 0;
    return AudioStreamOut::flush();
}

int SpdifStreamOut::standby()
{
    mRenderPositionHal = 0;
    mPreviousHalPosition32 = 0;
    return AudioStreamOut::standby();
}

// Account for possibly higher sample rate.
// This is much easier when all the values are 64-bit.
status_t SpdifStreamOut::getPresentationPosition(uint64_t *frames,
        struct timespec *timestamp)
{
    uint64_t halFrames = 0;
    status_t status = AudioStreamOut::getPresentationPosition(&halFrames, timestamp);
    *frames = halFrames / mRateMultiplier;
    return status;
}

size_t SpdifStreamOut::getFrameSize()
{
    return sizeof(int8_t);
}

ssize_t SpdifStreamOut::writeDataBurst(const void* buffer, size_t bytes)
{
    return AudioStreamOut::write(buffer, bytes);
}

ssize_t SpdifStreamOut::write(const void* buffer, size_t bytes)
{
    // Write to SPDIF wrapper. It will call back to writeDataBurst().
    return mSpdifEncoder.write(buffer, bytes);
}

} // namespace android
