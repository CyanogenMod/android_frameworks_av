/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "AudioStreamOutSink"
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <media/nbaio/AudioStreamOutSink.h>

namespace android {

AudioStreamOutSink::AudioStreamOutSink(audio_stream_out *stream) :
        NBAIO_Sink(),
        mStream(stream),
        mStreamBufferSizeBytes(0)
{
    ALOG_ASSERT(stream != NULL);
}

AudioStreamOutSink::~AudioStreamOutSink()
{
}

ssize_t AudioStreamOutSink::negotiate(const NBAIO_Format offers[], size_t numOffers,
                                      NBAIO_Format counterOffers[], size_t& numCounterOffers)
{
    if (mFormat == Format_Invalid) {
        mStreamBufferSizeBytes = mStream->common.get_buffer_size(&mStream->common);
        audio_format_t streamFormat = mStream->common.get_format(&mStream->common);
        if (streamFormat == AUDIO_FORMAT_PCM_16_BIT) {
            uint32_t sampleRate = mStream->common.get_sample_rate(&mStream->common);
            audio_channel_mask_t channelMask =
                    (audio_channel_mask_t) mStream->common.get_channels(&mStream->common);
            mFormat = Format_from_SR_C(sampleRate, popcount(channelMask));
            mBitShift = Format_frameBitShift(mFormat);
        }
    }
    return NBAIO_Sink::negotiate(offers, numOffers, counterOffers, numCounterOffers);
}

ssize_t AudioStreamOutSink::write(const void *buffer, size_t count)
{
    if (!mNegotiated) {
        return NEGOTIATE;
    }
    ALOG_ASSERT(mFormat != Format_Invalid);
    ssize_t ret = mStream->write(mStream, buffer, count << mBitShift);
    if (ret > 0) {
        ret >>= mBitShift;
        mFramesWritten += ret;
    } else {
        // FIXME verify HAL implementations are returning the correct error codes e.g. WOULD_BLOCK
    }
    return ret;
}

status_t AudioStreamOutSink::getNextWriteTimestamp(int64_t *timestamp) {
    ALOG_ASSERT(timestamp != NULL);

    if (NULL == mStream)
        return INVALID_OPERATION;
#ifndef ICS_AUDIO_BLOB
    if (NULL == mStream->get_next_write_timestamp)
        return INVALID_OPERATION;

    return mStream->get_next_write_timestamp(mStream, timestamp);
#else
    return INVALID_OPERATION;
#endif
}

}   // namespace android
