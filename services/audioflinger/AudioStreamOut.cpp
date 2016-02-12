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

#include "AudioHwDevice.h"
#include "AudioStreamOut.h"

namespace android {

// ----------------------------------------------------------------------------
AudioStreamOut::AudioStreamOut(AudioHwDevice *dev, audio_output_flags_t flags)
        : audioHwDev(dev)
        , stream(NULL)
        , flags(flags)
        , mFramesWritten(0)
        , mFramesWrittenAtStandby(0)
        , mRenderPosition(0)
        , mRateMultiplier(1)
        , mHalFormatHasProportionalFrames(false)
        , mHalFrameSize(0)
{
}

audio_hw_device_t *AudioStreamOut::hwDev() const
{
    return audioHwDev->hwDevice();
}

status_t AudioStreamOut::getRenderPosition(uint64_t *frames)
{
    if (stream == NULL) {
        return NO_INIT;
    }

    uint32_t halPosition = 0;
    status_t status = stream->get_render_position(stream, &halPosition);
    if (status != NO_ERROR) {
        return status;
    }

    // Maintain a 64-bit render position using the 32-bit result from the HAL.
    // This delta calculation relies on the arithmetic overflow behavior
    // of integers. For example (100 - 0xFFFFFFF0) = 116.
    uint32_t truncatedPosition = (uint32_t)mRenderPosition;
    int32_t deltaHalPosition = (int32_t)(halPosition - truncatedPosition);
    if (deltaHalPosition > 0) {
        mRenderPosition += deltaHalPosition;
    }
    // Scale from HAL sample rate to application rate.
    *frames = mRenderPosition / mRateMultiplier;

    return status;
}

// return bottom 32-bits of the render position
status_t AudioStreamOut::getRenderPosition(uint32_t *frames)
{
    uint64_t position64 = 0;
    status_t status = getRenderPosition(&position64);
    if (status == NO_ERROR) {
        *frames = (uint32_t)position64;
    }
    return status;
}

status_t AudioStreamOut::getPresentationPosition(uint64_t *frames, struct timespec *timestamp)
{
    if (stream == NULL) {
        return NO_INIT;
    }

    uint64_t halPosition = 0;
    status_t status = stream->get_presentation_position(stream, &halPosition, timestamp);
    if (status != NO_ERROR) {
        return status;
    }

    // Adjust for standby using HAL rate frames.
    // Only apply this correction if the HAL is getting PCM frames.
    if (mHalFormatHasProportionalFrames) {
        uint64_t adjustedPosition = (halPosition <= mFramesWrittenAtStandby) ?
                0 : (halPosition - mFramesWrittenAtStandby);
        // Scale from HAL sample rate to application rate.
        *frames = adjustedPosition / mRateMultiplier;
    } else {
        // For offloaded MP3 and other compressed formats.
        *frames = halPosition;
    }

    return status;
}

status_t AudioStreamOut::open(
        audio_io_handle_t handle,
        audio_devices_t devices,
        struct audio_config *config,
        const char *address)
{
    audio_stream_out_t *outStream;

    audio_output_flags_t customFlags = (config->format == AUDIO_FORMAT_IEC61937)
                ? (audio_output_flags_t)(flags | AUDIO_OUTPUT_FLAG_IEC958_NONAUDIO)
                : flags;

    int status = hwDev()->open_output_stream(
            hwDev(),
            handle,
            devices,
            customFlags,
            config,
            &outStream,
            address);
    ALOGV("AudioStreamOut::open(), HAL returned "
            " stream %p, sampleRate %d, Format %#x, "
            "channelMask %#x, status %d",
            outStream,
            config->sample_rate,
            config->format,
            config->channel_mask,
            status);

    // Some HALs may not recognize AUDIO_FORMAT_IEC61937. But if we declare
    // it as PCM then it will probably work.
    if (status != NO_ERROR && config->format == AUDIO_FORMAT_IEC61937) {
        struct audio_config customConfig = *config;
        customConfig.format = AUDIO_FORMAT_PCM_16_BIT;

        status = hwDev()->open_output_stream(
                hwDev(),
                handle,
                devices,
                customFlags,
                &customConfig,
                &outStream,
                address);
        ALOGV("AudioStreamOut::open(), treat IEC61937 as PCM, status = %d", status);
    }

    if (status == NO_ERROR) {
        stream = outStream;
        mHalFormatHasProportionalFrames = audio_has_proportional_frames(config->format);
        mHalFrameSize = audio_stream_out_frame_size(stream);
    }

    return status;
}

audio_format_t AudioStreamOut::getFormat() const
{
    return stream->common.get_format(&stream->common);
}

uint32_t AudioStreamOut::getSampleRate() const
{
    return stream->common.get_sample_rate(&stream->common);
}

audio_channel_mask_t AudioStreamOut::getChannelMask() const
{
    return stream->common.get_channels(&stream->common);
}

int AudioStreamOut::flush()
{
    ALOG_ASSERT(stream != NULL);
    mRenderPosition = 0;
    mFramesWritten = 0;
    mFramesWrittenAtStandby = 0;
    if (stream->flush != NULL) {
        return stream->flush(stream);
    }
    return NO_ERROR;
}

int AudioStreamOut::standby()
{
    ALOG_ASSERT(stream != NULL);
    mRenderPosition = 0;
    mFramesWrittenAtStandby = mFramesWritten;
    return stream->common.standby(&stream->common);
}

ssize_t AudioStreamOut::write(const void *buffer, size_t numBytes)
{
    ALOG_ASSERT(stream != NULL);
    ssize_t bytesWritten = stream->write(stream, buffer, numBytes);
    if (bytesWritten > 0 && mHalFrameSize > 0) {
        mFramesWritten += bytesWritten / mHalFrameSize;
    }
    return bytesWritten;
}

} // namespace android
