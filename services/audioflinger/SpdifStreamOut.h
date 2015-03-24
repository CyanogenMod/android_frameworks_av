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

#ifndef ANDROID_SPDIF_STREAM_OUT_H
#define ANDROID_SPDIF_STREAM_OUT_H

#include <stdint.h>
#include <sys/types.h>

#include <system/audio.h>

#include "AudioHwDevice.h"
#include "AudioStreamOut.h"
#include "SpdifStreamOut.h"

#include <audio_utils/spdif/SPDIFEncoder.h>

namespace android {

/**
 * Stream that is a PCM data burst in the HAL but looks like an encoded stream
 * to the AudioFlinger. Wraps encoded data in an SPDIF wrapper per IEC61973-3.
 */
class SpdifStreamOut : public AudioStreamOut {
public:

    SpdifStreamOut(AudioHwDevice *dev, audio_output_flags_t flags);

    virtual ~SpdifStreamOut() { }

    virtual status_t open(
            audio_io_handle_t handle,
            audio_devices_t devices,
            struct audio_config *config,
            const char *address);

    virtual status_t getRenderPosition(uint32_t *frames);

    virtual status_t getPresentationPosition(uint64_t *frames, struct timespec *timestamp);

    /**
    * Write audio buffer to driver. Returns number of bytes written, or a
    * negative status_t. If at least one frame was written successfully prior to the error,
    * it is suggested that the driver return that successful (short) byte count
    * and then return an error in the subsequent call.
    *
    * If set_callback() has previously been called to enable non-blocking mode
    * the write() is not allowed to block. It must write only the number of
    * bytes that currently fit in the driver/hardware buffer and then return
    * this byte count. If this is less than the requested write size the
    * callback function must be called when more space is available in the
    * driver/hardware buffer.
    */
    virtual ssize_t write(const void* buffer, size_t bytes);

    virtual size_t getFrameSize();

    virtual status_t flush();
    virtual status_t standby();

private:

    class MySPDIFEncoder : public SPDIFEncoder
    {
    public:
        MySPDIFEncoder(SpdifStreamOut *spdifStreamOut)
          : mSpdifStreamOut(spdifStreamOut)
        {
        }

        virtual ssize_t writeOutput(const void* buffer, size_t bytes)
        {
            return mSpdifStreamOut->writeDataBurst(buffer, bytes);
        }
    protected:
        SpdifStreamOut * const mSpdifStreamOut;
    };

    int                  mRateMultiplier;
    MySPDIFEncoder       mSpdifEncoder;

    // Used to implement getRenderPosition()
    int64_t              mRenderPositionHal;
    uint32_t             mPreviousHalPosition32;

    ssize_t  writeDataBurst(const void* data, size_t bytes);
    ssize_t  writeInternal(const void* buffer, size_t bytes);

};

} // namespace android

#endif // ANDROID_SPDIF_STREAM_OUT_H
