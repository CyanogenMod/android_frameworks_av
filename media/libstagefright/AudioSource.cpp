/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include <inttypes.h>
#include <stdlib.h>

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
#include <system/audio.h>

#define AUDIO_RECORD_DEFAULT_BUFFER_DURATION 20
namespace android {

static void AudioRecordCallbackFunction(int event, void *user, void *info) {
    AudioSource *source = (AudioSource *) user;
    source->onEvent(event, info);
}

AudioSource::AudioSource(
        audio_source_t inputSource, uint32_t sampleRate, uint32_t channelCount)
    : mStarted(false),
      mSampleRate(sampleRate),
      mPrevSampleTimeUs(0),
      mNumFramesReceived(0),
      mFormat(AUDIO_FORMAT_PCM_16_BIT),
      mMime(MEDIA_MIMETYPE_AUDIO_RAW),
      mMaxBufferSize(kMaxBufferSize),
      mNumClientOwnedBuffers(0),
      mRecPaused(false) {
    ALOGV("sampleRate: %d, channelCount: %d", sampleRate, channelCount);
    CHECK(channelCount == 1 || channelCount == 2 || channelCount == 6);

    size_t minFrameCount;
    status_t status = AudioRecord::getMinFrameCount(&minFrameCount,
                                           sampleRate,
                                           AUDIO_FORMAT_PCM_16_BIT,
                                           audio_channel_in_mask_from_count(channelCount));
    if (status == OK) {
        // make sure that the AudioRecord callback never returns more than the maximum
        // buffer size
        uint32_t frameCount = kMaxBufferSize / sizeof(int16_t) / channelCount;

        // make sure that the AudioRecord total buffer size is large enough
        size_t bufCount = 2;
        while ((bufCount * frameCount) < minFrameCount) {
            bufCount++;
        }

        mTempBuf.size = 0;
        mTempBuf.frameCount = 0;
        mTempBuf.i16 = (short*)NULL;
        mPrevPosition = 0;
        mAudioSessionId = -1;
        mAllocBytes = 0;
        mTransferMode = AudioRecord::TRANSFER_CALLBACK;

        //decide whether to use callback or event pos callback
        //use position marker only for PCM 16 bit and mono or stereo capture
        //and if input source is camera
        if((mFormat == AUDIO_FORMAT_PCM_16_BIT) &&
            ((channelCount == 1) || (channelCount == 2)) &&
            (inputSource == AUDIO_SOURCE_CAMCORDER)) {

            //Need audioSession Id in the extended audio record constructor
            //where the transfer mode can be specified
            mAudioSessionId = AudioSystem::newAudioUniqueId();
            AudioSystem::acquireAudioSessionId(mAudioSessionId, -1);

            mRecord = new AudioRecord(
                        inputSource, sampleRate, AUDIO_FORMAT_PCM_16_BIT,
                        audio_channel_in_mask_from_count(channelCount),
                        (size_t) (bufCount * frameCount),
                        AudioRecordCallbackFunction,
                        this,
                        frameCount /*notificationFrames*/,
                        mAudioSessionId,
                        AudioRecord::TRANSFER_SYNC,
                        AUDIO_INPUT_FLAG_NONE);

            int buffDuration = AUDIO_RECORD_DEFAULT_BUFFER_DURATION;
            char propValue[PROPERTY_VALUE_MAX];
            if (property_get("audio.record.buffer.duration", propValue, NULL)) {
                if (atoi(propValue) < AUDIO_RECORD_DEFAULT_BUFFER_DURATION)
                    buffDuration = AUDIO_RECORD_DEFAULT_BUFFER_DURATION;
                else
                    buffDuration = atoi(propValue);
            }
            else
                buffDuration = AUDIO_RECORD_DEFAULT_BUFFER_DURATION;

            //set to update position after frames worth of buffduration time for 16 bits
            mAllocBytes = ((sizeof(uint8_t) * frameCount * 2 * channelCount));
            ALOGI("AudioSource in TRANSFER_SYNC with duration %d ms", buffDuration);
            mTempBuf.i16 = (short*) malloc(mAllocBytes);
            if (mTempBuf.i16 == NULL) {
                mAllocBytes = 0;
                mInitCheck = NO_MEMORY;
            }
            mTransferMode = AudioRecord::TRANSFER_SYNC;
            mRecord->setPositionUpdatePeriod((sampleRate * buffDuration)/1000);
        } else {
            //Sound recorder and VOIP use cases does NOT use aggregation
            mRecord = new AudioRecord(
                        inputSource, sampleRate, AUDIO_FORMAT_PCM_16_BIT,
                        audio_channel_in_mask_from_count(channelCount),
                        (size_t) (bufCount * frameCount),
                        AudioRecordCallbackFunction,
                        this,
                        frameCount /*notificationFrames*/);
            ALOGI("AudioSource in TRANSFER_CALLBACK");
            mTransferMode = AudioRecord::TRANSFER_CALLBACK;
        }

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
    ALOGV("mInitCheck %d", mInitCheck);
}

AudioSource::AudioSource( audio_source_t inputSource, const sp<MetaData>& meta )
    : mStarted(false),
      mPrevSampleTimeUs(0),
      mNumFramesReceived(0),
      mNumClientOwnedBuffers(0),
      mFormat(AUDIO_FORMAT_PCM_16_BIT),
      mMime(MEDIA_MIMETYPE_AUDIO_RAW),
      mRecPaused(false) {

    const char * mime;
    ALOGV("AudioSource CTOR compress offload capture: inputSource: %d", inputSource);
    CHECK(meta->findCString(kKeyMIMEType, &mime));
    mMime = mime;
    int32_t sampleRate = 0;
    int32_t channels = 0;
    CHECK(meta->findInt32(kKeyChannelCount, &channels));
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
    mSampleRate = sampleRate;
    if (!strcasecmp( mime, MEDIA_MIMETYPE_AUDIO_AMR_WB)) {
        mFormat = AUDIO_FORMAT_AMR_WB;
        mMaxBufferSize = AMR_WB_FRAMESIZE*10;
    } else {
        CHECK(0);
    }
    mAutoRampStartUs = 0;
    CHECK(channels == 1 || channels == 2);

    mRecord = new AudioRecord(
                inputSource, sampleRate, mFormat,
                channels > 1? AUDIO_CHANNEL_IN_STEREO:
                AUDIO_CHANNEL_IN_MONO,
                4* mMaxBufferSize/channels,
                AudioRecordCallbackFunction,
                this);
    mInitCheck = mRecord->initCheck();
    mTempBuf.size = 0;
    mTempBuf.frameCount = 0;
    mTempBuf.i16 = (short*)NULL;
    mPrevPosition = 0;
    mAudioSessionId = -1;
    mAllocBytes = 0;
    mTransferMode = AudioRecord::TRANSFER_CALLBACK;

}

AudioSource::~AudioSource() {
    if (mStarted) {
        reset();
    }

    if (mTransferMode == AudioRecord::TRANSFER_SYNC) {
        if(mTempBuf.i16) {
            free(mTempBuf.i16);
            mTempBuf.i16 = (short*)NULL;
        }
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
    ALOGV("waitOutstandingEncodingFrames_l: %" PRId64, mNumClientOwnedBuffers);
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
    mFrameAvailableCondition.signal();

    mRecord->stop();
    waitOutstandingEncodingFrames_l();
    releaseQueuedFrames_l();

    if (mTransferMode == AudioRecord::TRANSFER_SYNC) {
        if(mAudioSessionId != -1)
            AudioSystem::releaseAudioSessionId(mAudioSessionId, -1);

        mAudioSessionId = -1;
        mTempBuf.size = 0;
        mTempBuf.frameCount = 0;
    }
    return OK;
}

sp<MetaData> AudioSource::getFormat() {
    Mutex::Autolock autoLock(mLock);
    if (mInitCheck != OK) {
        return 0;
    }

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, mMime);
    meta->setInt32(kKeySampleRate, mSampleRate);
    meta->setInt32(kKeyChannelCount, mRecord->channelCount());
    meta->setInt32(kKeyMaxInputSize, mMaxBufferSize);

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
        MediaBuffer **out, const ReadOptions * /* options */) {
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
    if ( mFormat == AUDIO_FORMAT_PCM_16_BIT ) {
        if (elapsedTimeUs < mAutoRampStartUs) {
            memset((uint8_t *) buffer->data(), 0, buffer->range_length());
        } else if (elapsedTimeUs < mAutoRampStartUs + kAutoRampDurationUs) {
            int32_t autoRampDurationFrames =
                    ((int64_t)kAutoRampDurationUs * mSampleRate + 500000LL) / 1000000LL;

            int32_t autoRampStartFrames =
                    ((int64_t)kAutoRampStartUs * mSampleRate + 500000LL) / 1000000LL;

            int32_t nFrames = mNumFramesReceived - autoRampStartFrames;
            rampVolume(nFrames, autoRampDurationFrames,
                    (uint8_t *) buffer->data(), buffer->range_length());
        }
    }

    // Track the max recording signal amplitude.
    if (mTrackMaxAmplitude && ( mFormat == AUDIO_FORMAT_PCM_16_BIT)) {
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

    ALOGV("dataCallbackTimestamp: %" PRId64 " us", timeUs);
    Mutex::Autolock autoLock(mLock);
    if (!mStarted) {
        ALOGW("Spurious callback from AudioRecord. Drop the audio data.");
        return OK;
    }

    // Drop retrieved and previously lost audio data.
    if (mNumFramesReceived == 0 && timeUs < mStartTimeUs) {
        (void) mRecord->getInputFramesLost();
        ALOGV("Drop audio data at %" PRId64 "/%" PRId64 " us", timeUs, mStartTimeUs);
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
    if ( mFormat == AUDIO_FORMAT_PCM_16_BIT )
        CHECK_EQ(audioBuffer.size & 1, 0u);
    if (numLostBytes > 0) {
        // Loss of audio frames should happen rarely; thus the LOGW should
        // not cause a logging spam
        ALOGW("Lost audio record data: %zu bytes", numLostBytes);
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

void AudioSource::onEvent(int event, void* info) {

    switch (event) {
        case AudioRecord::EVENT_MORE_DATA: {
            dataCallback(*((AudioRecord::Buffer *) info));
            break;
        }
        case AudioRecord::EVENT_NEW_POS: {
            uint32_t position = 0;
            mRecord->getPosition(&position);
            size_t framestoRead = position - mPrevPosition;
            size_t bytestoRead = (framestoRead * 2 * mRecord->channelCount());
            if(bytestoRead <=0 || bytestoRead > mAllocBytes) {
                //try to read only max
                ALOGI("greater than allocated size in callback, adjusting size");
                bytestoRead =  mAllocBytes;
                framestoRead = (mAllocBytes / (2 * mRecord->channelCount()));
            }

            if(mTempBuf.i16 && framestoRead > 0) {
                //read only if you have valid data
                size_t bytesRead = mRecord->read(mTempBuf.i16, framestoRead);
                size_t framesRead = 0;
                ALOGV("event_new_pos, new pos %d, frames to read %d\n", \
                        position, framestoRead);
                ALOGV("bytes read = %d \n", bytesRead);
                if(bytesRead > 0){
                    framesRead = (bytesRead / (2 * mRecord->channelCount()));
                    mPrevPosition += framesRead;
                    mTempBuf.size = bytesRead;
                    mTempBuf.frameCount = framesRead;
                    dataCallback(mTempBuf);
                } else {
                    ALOGE("EVENT_NEW_POS did not return any data");
                }
            } else {
                ALOGE("Init error");
            }
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
    return;
}

void AudioSource::queueInputBuffer_l(MediaBuffer *buffer, int64_t timeUs) {
    if (mRecPaused) {
        if (!mBuffersReceived.empty()) {
            releaseQueuedFrames_l();
        }
        buffer->release();
        return;
    }

    const size_t bufferSize = buffer->range_length();
    const size_t frameSize = mRecord->frameSize();
    int64_t timestampUs = mPrevSampleTimeUs;
    int64_t recordDurationUs = 0;
    if ( mFormat == AUDIO_FORMAT_PCM_16_BIT && mSampleRate){
        recordDurationUs = ((1000000LL * (bufferSize / (2 * mRecord->channelCount()))) +
                    (mSampleRate >> 1)) / mSampleRate;
    } else if ( mFormat == AUDIO_FORMAT_AMR_WB) {
       recordDurationUs = (bufferSize/AMR_WB_FRAMESIZE)*20*1000;//20ms
    }
    timestampUs += recordDurationUs;

    if (mNumFramesReceived == 0) {
        buffer->meta_data()->setInt64(kKeyAnchorTime, mStartTimeUs);
    }

    buffer->meta_data()->setInt64(kKeyTime, mPrevSampleTimeUs);
    if (mFormat == AUDIO_FORMAT_PCM_16_BIT) {
        buffer->meta_data()->setInt64(kKeyDriftTime, timeUs - mInitialReadTimeUs);
    } else {
        int64_t wallClockTimeUs = timeUs - mInitialReadTimeUs;
        int64_t mediaTimeUs = mStartTimeUs + mPrevSampleTimeUs;
        buffer->meta_data()->setInt64(kKeyDriftTime, mediaTimeUs - wallClockTimeUs);
    }
    mPrevSampleTimeUs = timestampUs;
    if (mFormat == AUDIO_FORMAT_AMR_WB)
        mNumFramesReceived += buffer->range_length() / AMR_WB_FRAMESIZE;
    else
        mNumFramesReceived += buffer->range_length() / sizeof(int16_t);
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

}  // namespace android
