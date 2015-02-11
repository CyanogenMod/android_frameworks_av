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
{
}

audio_hw_device_t* AudioStreamOut::hwDev() const
{
    return audioHwDev->hwDevice();
}

status_t AudioStreamOut::getRenderPosition(uint32_t *frames)
{
    if (stream == NULL) {
        return NO_INIT;
    }
    return stream->get_render_position(stream, frames);
}

status_t AudioStreamOut::getPresentationPosition(uint64_t *frames, struct timespec *timestamp)
{
    if (stream == NULL) {
        return NO_INIT;
    }
    return stream->get_presentation_position(stream, frames, timestamp);
}

status_t AudioStreamOut::open(
        audio_io_handle_t handle,
        audio_devices_t devices,
        struct audio_config *config,
        const char *address)
{
    audio_stream_out_t* outStream;
    int status = hwDev()->open_output_stream(
            hwDev(),
            handle,
            devices,
            flags,
            config,
            &outStream,
            address);
    ALOGV("AudioStreamOut::open(), HAL open_output_stream returned "
            " %p, sampleRate %d, Format %#x, "
            "channelMask %#x, status %d",
            outStream,
            config->sample_rate,
            config->format,
            config->channel_mask,
            status);

    if (status == NO_ERROR) {
        stream = outStream;
    }

    return status;
}

size_t AudioStreamOut::getFrameSize()
{
    ALOG_ASSERT(stream != NULL);
    return audio_stream_out_frame_size(stream);
}

int AudioStreamOut::flush()
{
    ALOG_ASSERT(stream != NULL);
    if (stream->flush != NULL) {
        return stream->flush(stream);
    }
    return NO_ERROR;
}

int AudioStreamOut::standby()
{
    ALOG_ASSERT(stream != NULL);
    return stream->common.standby(&stream->common);
}

ssize_t AudioStreamOut::write(const void* buffer, size_t bytes)
{
    ALOG_ASSERT(stream != NULL);
    return stream->write(stream, buffer, bytes);
}

} // namespace android
