/*
 * Copyright (C) 2010 The Android Open Source Project
 * Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 * Not a Contribution.
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

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioSource"
#include <utils/Log.h>

#include <media/AudioRecord.h>
#include <media/stagefright/AudioSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <cutils/properties.h>
#include <stdlib.h>
#ifdef ENABLE_AV_ENHANCEMENTS
#include <QCMediaDefs.h>
#endif
#include <system/audio.h>

#define DEFAULT_TUNNEL_BUFFER_COUNT 4

namespace android {

static void AudioRecordCallbackFunction(int event, void *user, void *info) {
    AudioSource *source = (AudioSource *) user;
    switch (event) {
        case AudioRecord::EVENT_MORE_DATA: {
            source->dataCallback(*((AudioRecord::Buffer *) info));
            break;
        }
        case AudioRecord::EVENT_OVERRUN: {
            ALOGW("AudioRecord reported overrun!");
            break;
        }
        default:
            // does nothing
            break;
    }
}

AudioSource::AudioSource(
        audio_source_t inputSource, uint32_t sampleRate, uint32_t channelCount)
#ifdef QCOM_DIRECTTRACK
    : mRecord(NULL),
      mStarted(false),
#else
    : mStarted(false),
#endif
      mSampleRate(sampleRate),
      mPrevSampleTimeUs(0),
      mRecPaused(false),
      mNumFramesReceived(0),
#ifdef QCOM_HARDWARE
      mNumClientOwnedBuffers(0),
      mFormat(AUDIO_FORMAT_PCM_16_BIT),
      mMime(MEDIA_MIMETYPE_AUDIO_RAW) {
#else
      mNumClientOwnedBuffers(0) {
#endif
    ALOGV("sampleRate: %d, channelCount: %d", sampleRate, channelCount);
#ifdef QCOM_HARDWARE
    CHECK(channelCount == 1 || channelCount == 2 || channelCount == 6);
#else
    CHECK(channelCount == 1 || channelCount == 2);
#endif

    size_t minFrameCount;
    status_t status = AudioRecord::getMinFrameCount(&minFrameCount,
                                           sampleRate,
                                           AUDIO_FORMAT_PCM_16_BIT,
                                           audio_channel_in_mask_from_count(channelCount));
#ifdef QCOM_HARDWARE
    if ( NO_ERROR != AudioSystem::getInputBufferSize(
        sampleRate, mFormat, channelCount, (size_t*)&mMaxBufferSize) ) {
        mMaxBufferSize = kMaxBufferSize;
        ALOGV("mMaxBufferSize = %d", mMaxBufferSize);
    }
#endif
    if (status == OK) {
        // make sure that the AudioRecord callback never returns more than the maximum
        // buffer size
#ifdef QCOM_HARDWARE
        int frameCount = mMaxBufferSize / sizeof(int16_t) / channelCount;
#else
        int frameCount = kMaxBufferSize / sizeof(int16_t) / channelCount;
#endif

        // make sure that the AudioRecord total buffer size is large enough
        int bufCount = 2;
        while ((bufCount * frameCount) < minFrameCount) {
            bufCount++;
        }

        mRecord = new AudioRecord(
                    inputSource, sampleRate, AUDIO_FORMAT_PCM_16_BIT,
                    audio_channel_in_mask_from_count(channelCount),
#ifdef QCOM_HARDWARE
                    DEFAULT_TUNNEL_BUFFER_COUNT * mMaxBufferSize / sizeof(int16_t), /* Enable ping-pong buffers */
#else
                    bufCount * frameCount,
#endif
                    AudioRecordCallbackFunction,
                    this,
                    frameCount);
        mInitCheck = mRecord->initCheck();
        mAutoRampStartUs = kAutoRampStartUs;
        uint32_t playbackLatencyMs = 0;
        if (AudioSystem::getOutputLatency(&playbackLatencyMs,
                                          AUDIO_STREAM_DEFAULT) == OK) {
            if (2*playbackLatencyMs*1000LL > kAutoRampStartUs) {
                mAutoRampStartUs = 2*playbackLatencyMs*1000LL;
            }
        }
        ALOGD("Start autoramp from %lld", mAutoRampStartUs);
    } else {
        mInitCheck = status;
    }
}

#ifdef QCOM_HARDWARE
AudioSource::AudioSource( audio_source_t inputSource, const sp<MetaData>& meta )
    : mStarted(false),
      mPrevSampleTimeUs(0),
      mNumFramesReceived(0),
      mNumClientOwnedBuffers(0),
      mFormat(AUDIO_FORMAT_PCM_16_BIT),
      mMime(MEDIA_MIMETYPE_AUDIO_RAW),
      mRecPaused(false) {

    const char * mime;
    ALOGE("SK: in AudioSource : inputSource: %d", inputSource);
    CHECK( meta->findCString( kKeyMIMEType, &mime ) );
    mMime = mime;
    int32_t sampleRate = 0; //these are the only supported values
    int32_t channels = 0;      //for the below tunnel formats
    CHECK( meta->findInt32( kKeyChannelCount, &channels ) );
    CHECK( meta->findInt32( kKeySampleRate, &sampleRate ) );
    mSampleRate = sampleRate;
#ifdef QCOM_DIRECTTRACK
    int32_t frameSize = -1;
    if ( !strcasecmp( mime, MEDIA_MIMETYPE_AUDIO_AMR_NB ) ) {
        mFormat = AUDIO_FORMAT_AMR_NB;
        frameSize = AMR_FRAMESIZE;
        mMaxBufferSize = AMR_FRAMESIZE*10;
    } else
#ifdef ENABLE_AV_ENHANCEMENTS
    if ( !strcasecmp( mime, MEDIA_MIMETYPE_AUDIO_QCELP ) ) {
        mFormat = AUDIO_FORMAT_QCELP;
        frameSize = QCELP_FRAMESIZE;
        mMaxBufferSize = QCELP_FRAMESIZE*10;
    }
    else if ( !strcasecmp( mime, MEDIA_MIMETYPE_AUDIO_EVRC ) ) {
        mFormat = AUDIO_FORMAT_EVRC;
        frameSize = EVRC_FRAMESIZE;
        mMaxBufferSize = EVRC_FRAMESIZE*10;
    } else
#endif
#endif
    if ( !strcasecmp( mime, MEDIA_MIMETYPE_AUDIO_AMR_WB ) ) {
        mFormat = AUDIO_FORMAT_AMR_WB;
#ifdef QCOM_DIRECTTRACK
        frameSize = AMR_WB_FRAMESIZE;
#endif
        mMaxBufferSize = AMR_WB_FRAMESIZE*10;
    }
    else {
        CHECK(0);
    }
    mAutoRampStartUs = 0;
    CHECK(channels == 1 || channels == 2);

    mRecord = new AudioRecord(
                inputSource, sampleRate, mFormat,
                channels > 1? AUDIO_CHANNEL_IN_STEREO:
                AUDIO_CHANNEL_IN_MONO,
#ifdef QCOM_DIRECTTRACK
                4*mMaxBufferSize/channels/frameSize,
#else
                4*mMaxBufferSize,
#endif
                AudioRecordCallbackFunction,
                this);
    mInitCheck = mRecord->initCheck();
}
#endif

AudioSource::~AudioSource() {
    if (mStarted) {
        reset();
    }
}

status_t AudioSource::initCheck() const {
    return mInitCheck;
}

status_t AudioSource::start(MetaData *params) {
    Mutex::Autolock autoLock(mLock);

    if (mRecPaused) {
        mRecPaused = false;
        return OK;
    }

    if (mStarted) {
        return UNKNOWN_ERROR;
    }

    if (mInitCheck != OK) {
        return NO_INIT;
    }

    mTrackMaxAmplitude = false;
    mMaxAmplitude = 0;
    mInitialReadTimeUs = 0;
    mStartTimeUs = 0;
    int64_t startTimeUs;
    if (params && params->findInt64(kKeyTime, &startTimeUs)) {
        mStartTimeUs = startTimeUs;
    }
    status_t err = mRecord->start();
    if (err == OK) {
        mStarted = true;
    } else {
        mRecord.clear();
    }


    return err;
}
status_t AudioSource::pause() {
    ALOGV("AudioSource::Pause");
    mRecPaused = true;
    return OK;
}

void AudioSource::releaseQueuedFrames_l() {
    ALOGV("releaseQueuedFrames_l");
    List<MediaBuffer *>::iterator it;
    while (!mBuffersReceived.empty()) {
        it = mBuffersReceived.begin();
        (*it)->release();
        mBuffersReceived.erase(it);
    }
}

void AudioSource::waitOutstandingEncodingFrames_l() {
    ALOGV("waitOutstandingEncodingFrames_l: %lld", mNumClientOwnedBuffers);
    while (mNumClientOwnedBuffers > 0) {
        mFrameEncodingCompletionCondition.wait(mLock);
    }
}

status_t AudioSource::reset() {
    Mutex::Autolock autoLock(mLock);

    if (!mStarted) {
        return UNKNOWN_ERROR;
    }

    if (mInitCheck != OK) {
        return NO_INIT;
    }

    mStarted = false;
    mRecord->stop();
    waitOutstandingEncodingFrames_l();
    releaseQueuedFrames_l();

    return OK;
}

sp<MetaData> AudioSource::getFormat() {
    Mutex::Autolock autoLock(mLock);
    if (mInitCheck != OK) {
        return 0;
    }

    sp<MetaData> meta = new MetaData;
#ifdef QCOM_HARDWARE
    meta->setCString(kKeyMIMEType, mMime);
    meta->setInt32(kKeySampleRate, mRecord->getSampleRate());
#else
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);
    meta->setInt32(kKeySampleRate, mSampleRate);
#endif
    meta->setInt32(kKeyChannelCount, mRecord->channelCount());
#ifdef QCOM_HARDWARE
    meta->setInt32(kKeyMaxInputSize, mMaxBufferSize);
#else
    meta->setInt32(kKeyMaxInputSize, kMaxBufferSize);
#endif

    return meta;
}

void AudioSource::rampVolume(
        int32_t startFrame, int32_t rampDurationFrames,
        uint8_t *data,   size_t bytes) {

    const int32_t kShift = 14;
    int32_t fixedMultiplier = (startFrame << kShift) / rampDurationFrames;
    const int32_t nChannels = mRecord->channelCount();
    int32_t stopFrame = startFrame + bytes / sizeof(int16_t);
    int16_t *frame = (int16_t *) data;
    if (stopFrame > rampDurationFrames) {
        stopFrame = rampDurationFrames;
    }

    while (startFrame < stopFrame) {
        if (nChannels == 1) {  // mono
            frame[0] = (frame[0] * fixedMultiplier) >> kShift;
            ++frame;
            ++startFrame;
        } else {               // stereo
            frame[0] = (frame[0] * fixedMultiplier) >> kShift;
            frame[1] = (frame[1] * fixedMultiplier) >> kShift;
            frame += 2;
            startFrame += 2;
        }

        // Update the multiplier every 4 frames
        if ((startFrame & 3) == 0) {
            fixedMultiplier = (startFrame << kShift) / rampDurationFrames;
        }
    }
}

status_t AudioSource::read(
        MediaBuffer **out, const ReadOptions *options) {
    Mutex::Autolock autoLock(mLock);
    *out = NULL;

    if (mInitCheck != OK) {
        return NO_INIT;
    }

    while (mStarted && mBuffersReceived.empty()) {
        mFrameAvailableCondition.wait(mLock);
    }
    if (!mStarted) {
        return OK;
    }
    MediaBuffer *buffer = *mBuffersReceived.begin();
    mBuffersReceived.erase(mBuffersReceived.begin());
    ++mNumClientOwnedBuffers;
    buffer->setObserver(this);
    buffer->add_ref();

    // Mute/suppress the recording sound
    int64_t timeUs;
    CHECK(buffer->meta_data()->findInt64(kKeyTime, &timeUs));
    int64_t elapsedTimeUs = timeUs - mStartTimeUs;
#ifdef QCOM_HARDWARE
    if ( mFormat == AUDIO_FORMAT_PCM_16_BIT ) {
#endif
        if (elapsedTimeUs < kAutoRampStartUs) {
            memset((uint8_t *) buffer->data(), 0, buffer->range_length());
        } else if (elapsedTimeUs < kAutoRampStartUs + kAutoRampDurationUs) {
            int32_t autoRampDurationFrames =
                    (kAutoRampDurationUs * mSampleRate + 500000LL) / 1000000LL;

            int32_t autoRampStartFrames =
                    (kAutoRampStartUs * mSampleRate + 500000LL) / 1000000LL;

            int32_t nFrames = mNumFramesReceived - autoRampStartFrames;
            rampVolume(nFrames, autoRampDurationFrames,
                (uint8_t *) buffer->data(), buffer->range_length());
        }
#ifdef QCOM_HARDWARE
    }
#endif

    // Track the max recording signal amplitude.
#ifdef QCOM_HARDWARE
    if (mTrackMaxAmplitude && ( mFormat == AUDIO_FORMAT_PCM_16_BIT)) {
#else
    if (mTrackMaxAmplitude) {
#endif
        trackMaxAmplitude(
            (int16_t *) buffer->data(), buffer->range_length() >> 1);
    }

    *out = buffer;
    return OK;
}

void AudioSource::signalBufferReturned(MediaBuffer *buffer) {
    ALOGV("signalBufferReturned: %p", buffer->data());
    Mutex::Autolock autoLock(mLock);
    --mNumClientOwnedBuffers;
    buffer->setObserver(0);
    buffer->release();
    mFrameEncodingCompletionCondition.signal();
    return;
}

status_t AudioSource::dataCallback(const AudioRecord::Buffer& audioBuffer) {
    int64_t timeUs = systemTime() / 1000ll;

    ALOGV("dataCallbackTimestamp: %lld us", timeUs);
    Mutex::Autolock autoLock(mLock);
    if (!mStarted) {
        ALOGW("Spurious callback from AudioRecord. Drop the audio data.");
        return OK;
    }

    // Drop retrieved and previously lost audio data.
    if (mNumFramesReceived == 0 && timeUs < mStartTimeUs) {
        mRecord->getInputFramesLost();
        ALOGV("Drop audio data at %lld/%lld us", timeUs, mStartTimeUs);
        return OK;
    }

    if (mNumFramesReceived == 0 && mPrevSampleTimeUs == 0) {
        mInitialReadTimeUs = timeUs;
        // Initial delay
        if (mStartTimeUs > 0) {
            mStartTimeUs = timeUs - mStartTimeUs;
        } else {
            // Assume latency is constant.
            mStartTimeUs += mRecord->latency() * 1000;
        }

        mPrevSampleTimeUs = mStartTimeUs;
    }

    size_t numLostBytes = 0;
    if (mNumFramesReceived > 0) {  // Ignore earlier frame lost
        // getInputFramesLost() returns the number of lost frames.
        // Convert number of frames lost to number of bytes lost.
        numLostBytes = mRecord->getInputFramesLost() * mRecord->frameSize();
    }

    CHECK_EQ(numLostBytes & 1, 0u);
#ifdef QCOM_HARDWARE
    if ( mFormat == AUDIO_FORMAT_PCM_16_BIT )
#endif
        CHECK_EQ(audioBuffer.size & 1, 0u);
    if (numLostBytes > 0) {
        // Loss of audio frames should happen rarely; thus the LOGW should
        // not cause a logging spam
        ALOGW("Lost audio record data: %d bytes", numLostBytes);
    }

    while (numLostBytes > 0) {
        size_t bufferSize = numLostBytes;
        if (numLostBytes > kMaxBufferSize) {
            numLostBytes -= kMaxBufferSize;
            bufferSize = kMaxBufferSize;
        } else {
            numLostBytes = 0;
        }
        MediaBuffer *lostAudioBuffer = new MediaBuffer(bufferSize);
        memset(lostAudioBuffer->data(), 0, bufferSize);
        lostAudioBuffer->set_range(0, bufferSize);
        queueInputBuffer_l(lostAudioBuffer, timeUs);
    }

    if (audioBuffer.size == 0) {
        ALOGW("Nothing is available from AudioRecord callback buffer");
        return OK;
    }

    const size_t bufferSize = audioBuffer.size;
    MediaBuffer *buffer = new MediaBuffer(bufferSize);
    memcpy((uint8_t *) buffer->data(),
            audioBuffer.i16, audioBuffer.size);
    buffer->set_range(0, bufferSize);
    queueInputBuffer_l(buffer, timeUs);
    return OK;
}

void AudioSource::queueInputBuffer_l(MediaBuffer *buffer, int64_t timeUs) {
    if (mRecPaused) {
        if (!mBuffersReceived.empty())
            releaseQueuedFrames_l();

        buffer->release();
        return;
    }

    const size_t bufferSize = buffer->range_length();
    const size_t frameSize = mRecord->frameSize();
#ifdef QCOM_HARDWARE
    int64_t timestampUs = mPrevSampleTimeUs;
    int64_t recordDurationUs = 0;
    if ( mFormat == AUDIO_FORMAT_PCM_16_BIT && mSampleRate){
        recordDurationUs = ((1000000LL * (bufferSize / (2 * mRecord->channelCount()))) +
                        (mSampleRate >> 1)) / mSampleRate;
    } else if ( mFormat == AUDIO_FORMAT_AMR_WB) {
       recordDurationUs = (bufferSize/AMR_WB_FRAMESIZE)*20;//20ms
    }
    timestampUs += recordDurationUs;
#else
    const int64_t timestampUs =
                mPrevSampleTimeUs +
                    ((1000000LL * (bufferSize / frameSize)) +
                        (mSampleRate >> 1)) / mSampleRate;
#endif

    if (mNumFramesReceived == 0) {
        buffer->meta_data()->setInt64(kKeyAnchorTime, mStartTimeUs);
    }

    buffer->meta_data()->setInt64(kKeyTime, mPrevSampleTimeUs);
#ifdef QCOM_HARDWARE
    if (mFormat == AUDIO_FORMAT_PCM_16_BIT) {
#endif
        buffer->meta_data()->setInt64(kKeyDriftTime, timeUs - mInitialReadTimeUs);
#ifdef QCOM_HARDWARE
    } else {
        int64_t wallClockTimeUs = timeUs - mInitialReadTimeUs;
        int64_t mediaTimeUs = mStartTimeUs + mPrevSampleTimeUs;
        buffer->meta_data()->setInt64(kKeyDriftTime, mediaTimeUs - wallClockTimeUs);
    }
#endif
    mPrevSampleTimeUs = timestampUs;
#ifdef QCOM_HARDWARE
    if (mFormat == AUDIO_FORMAT_AMR_WB)
        mNumFramesReceived += buffer->range_length() / AMR_WB_FRAMESIZE;
    else
        mNumFramesReceived += buffer->range_length() / sizeof(int16_t);
#else
    mNumFramesReceived += bufferSize / frameSize;
#endif
    mBuffersReceived.push_back(buffer);
    mFrameAvailableCondition.signal();
}

void AudioSource::trackMaxAmplitude(int16_t *data, int nSamples) {
    for (int i = nSamples; i > 0; --i) {
        int16_t value = *data++;
        if (value < 0) {
            value = -value;
        }
        if (mMaxAmplitude < value) {
            mMaxAmplitude = value;
        }
    }
}

int16_t AudioSource::getMaxAmplitude() {
    // First call activates the tracking.
    if (!mTrackMaxAmplitude) {
        mTrackMaxAmplitude = true;
    }
    int16_t value = mMaxAmplitude;
    mMaxAmplitude = 0;
    ALOGV("max amplitude since last call: %d", value);
    return value;
}

#ifdef QCOM_HARDWARE
int64_t AudioSource::bufferDurationUs( ssize_t n ) {
    int64_t dataDurationMs = 0;
    if (mFormat == AUDIO_FORMAT_AMR_NB) {
        dataDurationMs = (n/AMR_FRAMESIZE) * 20; //ms
    }
    else if (mFormat == AUDIO_FORMAT_EVRC) {
       dataDurationMs = (n/EVRC_FRAMESIZE) * 20; //ms
    }
    else if (mFormat == AUDIO_FORMAT_QCELP) {
        dataDurationMs = (n/QCELP_FRAMESIZE) * 20; //ms
    }
    else if (mFormat == AUDIO_FORMAT_AMR_WB) {
        dataDurationMs = (n/AMR_WB_FRAMESIZE) * 20; //ms
    }
    else
        CHECK(0);
    return dataDurationMs*1000LL;
}
#endif
}  // namespace android
