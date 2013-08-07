/*
 * Copyright (C) 2009 The Android Open Source Project
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

#ifndef AUDIO_SOURCE_H_

#define AUDIO_SOURCE_H_

#include <media/AudioRecord.h>
#include <media/AudioSystem.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <utils/List.h>
#include <utils/String8.h>
#include <system/audio.h>

namespace android {

class AudioRecord;

struct AudioSource : public MediaSource, public MediaBufferObserver {
    // Note that the "channels" parameter _is_ the number of channels,
    // _not_ a bitmask of audio_channels_t constants.
    AudioSource(
            audio_source_t inputSource,
            uint32_t sampleRate,
            uint32_t channels = 1);

    status_t initCheck() const;

    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop() { return reset(); }
    virtual sp<MetaData> getFormat();
    status_t pause();

    // Returns the maximum amplitude since last call.
    int16_t getMaxAmplitude();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options = NULL);

    status_t dataCallback(const AudioRecord::Buffer& buffer);
    virtual void signalBufferReturned(MediaBuffer *buffer);

protected:
    virtual ~AudioSource();

private:
    enum {
        kMaxBufferSize = 2048,

        // After the initial mute, we raise the volume linearly
        // over kAutoRampDurationUs.
        kAutoRampDurationUs = 300000,

        // This is the initial mute duration to suppress
        // the video recording signal tone
        kAutoRampStartUs = 0,
    };

    Mutex mLock;
    Condition mFrameAvailableCondition;
    Condition mFrameEncodingCompletionCondition;

    sp<AudioRecord> mRecord;
    status_t mInitCheck;
    bool mStarted;
    bool mRecPaused;
    int32_t mSampleRate;

    bool mTrackMaxAmplitude;
    int64_t mStartTimeUs;
    int16_t mMaxAmplitude;
    int64_t mPrevSampleTimeUs;
    int64_t mInitialReadTimeUs;
    int64_t mNumFramesReceived;
    int64_t mNumClientOwnedBuffers;
    int64_t mAutoRampStartUs;

    List<MediaBuffer * > mBuffersReceived;

    void trackMaxAmplitude(int16_t *data, int nSamples);

    // This is used to raise the volume from mute to the
    // actual level linearly.
    void rampVolume(
        int32_t startFrame, int32_t rampDurationFrames,
        uint8_t *data,   size_t bytes);

    void queueInputBuffer_l(MediaBuffer *buffer, int64_t timeUs);
    void releaseQueuedFrames_l();
    void waitOutstandingEncodingFrames_l();
    status_t reset();

    AudioSource(const AudioSource &);
    AudioSource &operator=(const AudioSource &);
#ifdef QCOM_HARDWARE
public:
    AudioSource(
        audio_source_t inputSource, const sp<MetaData>& meta );
private:
    audio_format_t mFormat;
    String8 mMime;
    int32_t mMaxBufferSize;
    int64_t bufferDurationUs( ssize_t n );
#endif
};

}  // namespace android

#endif  // AUDIO_SOURCE_H_
