/*
 * Copyright (C) ST-Ericsson SA 2012
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
 *
 * Author: Stefan Ekenberg (stefan.ekenberg@stericsson.com) for ST-Ericsson
 */

#define LOG_TAG "FMRadioSource"
#include <utils/Log.h>

#include <media/stagefright/FMRadioSource.h>
#include <media/AudioSystem.h>
#include <private/media/AudioTrackShared.h>
#include <cutils/compiler.h>

namespace android {

static const int kSampleRate = 48000;
static const audio_format_t kAudioFormat = AUDIO_FORMAT_PCM_16_BIT;
static const audio_channel_mask_t kChannelMask = AUDIO_CHANNEL_IN_STEREO;
static const int kBufferTimeoutMs = 3000;

FMRadioSource::FMRadioSource()
    : mInitCheck(NO_INIT),
      mStarted(false),
      mSessionId(AudioSystem::newAudioSessionId()) {

    // get FM Radio RX input
    audio_io_handle_t input = AudioSystem::getInput(AUDIO_SOURCE_FM_RX,
                                                    kSampleRate,
                                                    kAudioFormat,
                                                    kChannelMask,
                                                    mSessionId);
    if (input == 0) {
        ALOGE("Could not get audio input for FM Radio source");
        mInitCheck = UNKNOWN_ERROR;
        return;
    }

    // get frame count
    int frameCount = 0;
    status_t status = AudioRecord::getMinFrameCount(&frameCount, kSampleRate,
                                                    kAudioFormat, popcount(kChannelMask));
    if (status != NO_ERROR) {
        mInitCheck = status;
        return;
    }

    // create the IAudioRecord
    status = openRecord(frameCount, input);
    if (status != NO_ERROR) {
        mInitCheck = status;
        return;
    }

    AudioSystem::acquireAudioSessionId(mSessionId);

    mInitCheck = OK;
    return;
}

FMRadioSource::~FMRadioSource() {
    AudioSystem::releaseAudioSessionId(mSessionId);
}

status_t FMRadioSource::initCheck() const {
    return mInitCheck;
}

ssize_t FMRadioSource::readAt(off64_t offset, void *data, size_t size) {
    Buffer audioBuffer;

    if (!mStarted) {
        status_t err = mAudioRecord->start(AudioSystem::SYNC_EVENT_NONE, 0);
        if (err == OK) {
            mStarted = true;
        } else {
            ALOGE("Failed to start audio source");
            return 0;
        }
    }

    // acquire a strong reference on the IAudioRecord and IMemory so that they cannot be destroyed
    // while we are accessing the cblk
    sp<IAudioRecord> audioRecord = mAudioRecord;
    sp<IMemory> iMem = mCblkMemory;
    audio_track_cblk_t* cblk = mCblk;

    audioBuffer.frameCount = size / cblk->frameSize;

    status_t err = obtainBuffer(&audioBuffer);
    if (err != NO_ERROR) {
        ALOGE("Error obtaining an audio buffer, giving up (err:%d).", err);
        return 0;
    }

    memcpy(data, audioBuffer.data, audioBuffer.size);
    mCblk->stepUser(audioBuffer.frameCount);

    return audioBuffer.size;
}

status_t FMRadioSource::getSize(off64_t *size) {
    *size = 0;
    return OK;
}

// -------------------------------------------------------------------------

status_t FMRadioSource::openRecord(int frameCount, audio_io_handle_t input)
{
    status_t status;
    const sp<IAudioFlinger>& audioFlinger = AudioSystem::get_audio_flinger();
    if (audioFlinger == 0) {
        return NO_INIT;
    }

    pid_t tid = gettid(); // or -1;

    sp<IAudioRecord> record = audioFlinger->openRecord(input,
                                                       kSampleRate,
                                                       kAudioFormat,
                                                       kChannelMask,
                                                       frameCount,
                                                       IAudioFlinger::TRACK_DEFAULT,
                                                       tid,
                                                       &mSessionId,
                                                       &status);

    if (record == 0) {
        ALOGE("AudioFlinger could not create record track, status: %d", status);
        return status;
    }

    sp<IMemory> cblk = record->getCblk();
    if (cblk == 0) {
        ALOGE("Could not get control block");
        return NO_INIT;
    }
    mAudioRecord = record;
    mCblkMemory = cblk;
    mCblk = static_cast<audio_track_cblk_t*>(cblk->pointer());
    mCblk->buffers = (char*)mCblk + sizeof(audio_track_cblk_t);
    android_atomic_and(~CBLK_DIRECTION_MSK, &mCblk->flags);
    return NO_ERROR;
}

status_t FMRadioSource::obtainBuffer(Buffer* audioBuffer)
{
    status_t result = NO_ERROR;
    uint32_t framesReq = audioBuffer->frameCount;

    audioBuffer->frameCount = 0;
    audioBuffer->size       = 0;

    mCblk->lock.lock();
    uint32_t framesReady = mCblk->framesReady();
    if (framesReady == 0) {
        do {
            result = mCblk->cv.waitRelative(mCblk->lock, milliseconds(kBufferTimeoutMs));
            if (CC_UNLIKELY(result != NO_ERROR)) {
                ALOGE("obtainBuffer timed out (is the CPU pegged?) "
                        "user=%08x, server=%08x", mCblk->user, mCblk->server);
                mCblk->lock.unlock();
                return TIMED_OUT;
            }

            framesReady = mCblk->framesReady();
        } while (framesReady == 0);
    }
    mCblk->lock.unlock();

    if (framesReq > framesReady) {
        framesReq = framesReady;
    }

    uint32_t u = mCblk->user;
    uint32_t bufferEnd = mCblk->userBase + mCblk->frameCount;

    if (framesReq > bufferEnd - u) {
        framesReq = bufferEnd - u;
    }

    audioBuffer->frameCount = framesReq;
    audioBuffer->size       = framesReq * mCblk->frameSize;
    audioBuffer->data       = (int8_t*)mCblk->buffer(u);

    return NO_ERROR;
}

}  // namespace android
