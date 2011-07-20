/*
 * Copyright (C) 2011 The Android Open Source Project
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

#define LOG_NDEBUG 1
#define LOG_TAG "VEAudioSource"
#include <utils/Log.h>


#include "VideoEditorSRC.h"
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include "AudioMixer.h"


namespace android {

VideoEditorSRC::VideoEditorSRC(
        const sp<MediaSource> &source) {

    LOGV("VideoEditorSRC::Create");
    mSource = source;
    mResampler = NULL;
    mBitDepth = 16;
    mChannelCnt = 0;
    mSampleRate = 0;
    mOutputSampleRate = DEFAULT_SAMPLING_FREQ;
    mStarted = false;
    mIsResamplingRequired = false;
    mIsChannelConvertionRequired = false;
    mInitialTimeStampUs = -1;
    mAccuOutBufferSize  = 0;
    mSeekTimeUs = -1;
    mLeftover = 0;
    mLastReadSize = 0;
    mReSampledBuffer = NULL;
    mSeekMode =  ReadOptions::SEEK_PREVIOUS_SYNC;

    mOutputFormat = new MetaData;

    // Input Source validation
    sp<MetaData> format = mSource->getFormat();
    const char *mime;
    bool success = format->findCString(kKeyMIMEType, &mime);
    CHECK(success);
    CHECK(!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW));

    //set the meta data of the output after convertion.
    if(mOutputFormat != NULL) {
        mOutputFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);
        mOutputFormat->setInt32(kKeySampleRate, DEFAULT_SAMPLING_FREQ);

        //by default we convert all data to stereo
        mOutputFormat->setInt32(kKeyChannelCount, 2);
    } else {
        LOGE("Meta data was not allocated.");
    }

    // Allocate a  1 sec buffer (test only, to be refined)
    mInterframeBufferPosition = 0;
    mInterframeBuffer = new uint8_t[DEFAULT_SAMPLING_FREQ * 2 * 2]; //stereo=2 * bytespersample=2


}

VideoEditorSRC::~VideoEditorSRC(){
    if (mStarted == true)
        stop();

    if(mOutputFormat != NULL) {
        mOutputFormat.clear();
        mOutputFormat = NULL;
    }

    if (mInterframeBuffer != NULL){
        delete mInterframeBuffer;
        mInterframeBuffer = NULL;
    }
}

void VideoEditorSRC::setResampling(int32_t sampleRate) {
    Mutex::Autolock autoLock(mLock);
    LOGV("VideoEditorSRC::setResampling called with samplreRate = %d", sampleRate);
    if(sampleRate != DEFAULT_SAMPLING_FREQ) { //default case
        LOGV("VideoEditor Audio resampler, freq set is other than default");
        CHECK(mOutputFormat->setInt32(kKeySampleRate, DEFAULT_SAMPLING_FREQ));
    }
    mOutputSampleRate = sampleRate;
    return;
}

status_t  VideoEditorSRC::start (MetaData *params) {
    Mutex::Autolock autoLock(mLock);

    CHECK(!mStarted);
    LOGV(" VideoEditorSRC:start() called");

    // Set resampler if required
    status_t err = checkAndSetResampler();
    if (err != OK) {
        LOGE("checkAndSetResampler() returned error %d", err);
        return err;
    }

    mSeekTimeUs = -1;
    mSeekMode =  ReadOptions::SEEK_PREVIOUS_SYNC;
    mStarted = true;
    mSource->start();

    return OK;
}

status_t VideoEditorSRC::stop() {

    Mutex::Autolock autoLock(mLock);
    LOGV("VideoEditorSRC::stop()");
    mSource->stop();
    if(mResampler != NULL) {
        delete mResampler;
        mResampler = NULL;
    }
    mStarted = false;
    mInitialTimeStampUs = -1;
    mAccuOutBufferSize  = 0;
    mLeftover = 0;
    mLastReadSize = 0;
    if (mReSampledBuffer != NULL) {
        free(mReSampledBuffer);
        mReSampledBuffer = NULL;
    }

    return OK;
}

sp<MetaData> VideoEditorSRC::getFormat() {
    LOGV("AudioSRC getFormat");
    //Mutex::Autolock autoLock(mLock);
    return mOutputFormat;
}

status_t VideoEditorSRC::read (
        MediaBuffer **buffer_out, const ReadOptions *options) {
    Mutex::Autolock autoLock(mLock);
    *buffer_out = NULL;
    int32_t leftover = 0;

    LOGV("VideoEditorSRC::read");

    if (!mStarted) {
        return ERROR_END_OF_STREAM;
    }

    if(mIsResamplingRequired == true) {

        LOGV("mIsResamplingRequired = true");

        // Store the seek parameters
        int64_t seekTimeUs;
        ReadOptions::SeekMode mode = ReadOptions::SEEK_PREVIOUS_SYNC;
        if (options && options->getSeekTo(&seekTimeUs, &mode)) {
            LOGV("read Seek %lld", seekTimeUs);
            mInitialTimeStampUs = -1;
            mSeekTimeUs = seekTimeUs;
            mSeekMode = mode;
        }

        // We ask for 1024 frames in output
        size_t outFrameCnt = 1024;
        int32_t outBufferSize = (outFrameCnt) * 2 * sizeof(int16_t); //out is always 2 channels & 16 bits
        int64_t outDurationUs = (outBufferSize * 1000000) /(mOutputSampleRate * 2 * sizeof(int16_t)); //2 channels out * 2 bytes per sample
        LOGV("outBufferSize            %d", outBufferSize);
        LOGV("outFrameCnt              %d", outFrameCnt);

        int32_t *pTmpBuffer = (int32_t*)malloc(outFrameCnt * 2 * sizeof(int32_t)); //out is always 2 channels and resampler out is 32 bits
        memset(pTmpBuffer, 0x00, outFrameCnt * 2 * sizeof(int32_t));
        // Resample to target quality
        mResampler->resample(pTmpBuffer, outFrameCnt, this);

        // Free previous allocation
        if (mReSampledBuffer != NULL) {
            free(mReSampledBuffer);
            mReSampledBuffer = NULL;
        }
        mReSampledBuffer = (int16_t*)malloc(outBufferSize);
        memset(mReSampledBuffer, 0x00, outBufferSize);

        // Convert back to 16 bits
        AudioMixer::ditherAndClamp((int32_t*)mReSampledBuffer, pTmpBuffer, outFrameCnt);
        LOGV("Resampled buffer size %d", outFrameCnt* 2 * sizeof(int16_t));

        // Create new MediaBuffer
        mCopyBuffer = new MediaBuffer((void*)mReSampledBuffer, outBufferSize);

        // Compute and set the new timestamp
        sp<MetaData> to = mCopyBuffer->meta_data();
        int64_t totalOutDurationUs = (mAccuOutBufferSize * 1000000) /(mOutputSampleRate * 2 * 2); //2 channels out * 2 bytes per sample
        int64_t timeUs = mInitialTimeStampUs + totalOutDurationUs;
        to->setInt64(kKeyTime, timeUs);
        LOGV("buffer duration %lld   timestamp %lld   init %lld", outDurationUs, timeUs, mInitialTimeStampUs);

        // update the accumulate size
        mAccuOutBufferSize += outBufferSize;

        mCopyBuffer->set_range(0, outBufferSize);
        *buffer_out = mCopyBuffer;

        free(pTmpBuffer);

    } else if(mIsChannelConvertionRequired == true) {
        //TODO convert to stereo here.
    } else {
        //LOGI("Resampling not required");
        MediaBuffer *aBuffer;
        status_t err = mSource->read(&aBuffer, options);
        LOGV("mSource->read returned %d", err);
        if(err != OK) {
            *buffer_out = NULL;
            mStarted = false;
            return err;
        }
        *buffer_out = aBuffer;
    }

    return OK;
}

status_t VideoEditorSRC::getNextBuffer(AudioBufferProvider::Buffer *pBuffer) {
    LOGV("Requesting        %d", pBuffer->frameCount);
    uint32_t availableFrames;
    bool lastBuffer = false;
    MediaBuffer *aBuffer;

    //update the internal buffer
    // Store the leftover at the beginning of the local buffer
    if (mLeftover > 0) {
        LOGV("Moving mLeftover =%d  from  %d", mLeftover, mLastReadSize);
        if (mLastReadSize > 0) {
            memcpy(mInterframeBuffer, (uint8_t*) (mInterframeBuffer + mLastReadSize), mLeftover);
        }
        mInterframeBufferPosition = mLeftover;
    }
    else {
        mInterframeBufferPosition = 0;
    }

    availableFrames = mInterframeBufferPosition / (mChannelCnt*2);

    while ((availableFrames < pBuffer->frameCount)&&(mStarted)) {
        // if we seek, reset the initial time stamp and accumulated time
        ReadOptions options;
        if (mSeekTimeUs >= 0) {
            LOGV("%p cacheMore_l Seek requested = %lld", this, mSeekTimeUs);
            ReadOptions::SeekMode mode = mSeekMode;
            options.setSeekTo(mSeekTimeUs, mode);
            mSeekTimeUs = -1;
        }
        /* The first call to read() will require to buffer twice as much data */
        /* This will be needed by the resampler */
        status_t err = mSource->read(&aBuffer, &options);
        LOGV("mSource->read returned %d", err);
        if (err == INFO_FORMAT_CHANGED) {
            LOGV("getNextBuffer: source read returned INFO_FORMAT_CHANGED");
            // Change resampler if required
            status_t err1 = checkAndSetResampler();
            if (err1 != OK) {
                LOGE("checkAndSetResampler() returned error %d", err1);
                return err1;
            }
            // Update availableFrames with new channel count
            availableFrames = mInterframeBufferPosition / (mChannelCnt*2);
            continue;
        } else if (err != OK) {
            if (mInterframeBufferPosition == 0) {
                mStarted = false;
            }
            //Empty the internal buffer if there is no more data left in the source
            else {
                lastBuffer = true;
                //clear the end of the buffer, just in case
                memset(mInterframeBuffer+mInterframeBufferPosition, 0x00, DEFAULT_SAMPLING_FREQ * 2 * 2 - mInterframeBufferPosition);
                mStarted = false;
            }
        }
        else {
            //copy the buffer
            memcpy(((uint8_t*) mInterframeBuffer) + mInterframeBufferPosition,
                    ((uint8_t*) aBuffer->data()) + aBuffer->range_offset(),
                    aBuffer->range_length());
            LOGV("Read from buffer  %d", aBuffer->range_length());

            mInterframeBufferPosition += aBuffer->range_length();
            LOGV("Stored            %d", mInterframeBufferPosition);

            // Get the time stamp of the first buffer
            if (mInitialTimeStampUs == -1) {
                int64_t curTS;
                sp<MetaData> from = aBuffer->meta_data();
                from->findInt64(kKeyTime, &curTS);
                LOGV("setting mInitialTimeStampUs to %lld", mInitialTimeStampUs);
                mInitialTimeStampUs = curTS;
            }

            // release the buffer
            aBuffer->release();
        }
        availableFrames = mInterframeBufferPosition / (mChannelCnt*2);
        LOGV("availableFrames   %d", availableFrames);
    }

    if (lastBuffer) {
        pBuffer->frameCount = availableFrames;
    }

    //update the input buffer
    pBuffer->raw        = (void*)(mInterframeBuffer);

    // Update how many bytes are left
    // (actualReadSize is updated in getNextBuffer() called from resample())
    int32_t actualReadSize = pBuffer->frameCount * mChannelCnt * 2;
    mLeftover = mInterframeBufferPosition - actualReadSize;
    LOGV("mLeftover         %d", mLeftover);

    mLastReadSize = actualReadSize;

    LOGV("inFrameCount     %d", pBuffer->frameCount);

    return OK;
}


void VideoEditorSRC::releaseBuffer(AudioBufferProvider::Buffer *pBuffer) {
    if(pBuffer->raw != NULL) {
        pBuffer->raw = NULL;
    }
    pBuffer->frameCount = 0;
}

status_t VideoEditorSRC::checkAndSetResampler() {

    LOGV("checkAndSetResampler");

    sp<MetaData> format = mSource->getFormat();
    const char *mime;
    bool success = format->findCString(kKeyMIMEType, &mime);
    CHECK(success);
    CHECK(!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW));

    success = format->findInt32(kKeySampleRate, &mSampleRate);
    CHECK(success);

    int32_t numChannels;
    success = format->findInt32(kKeyChannelCount, &mChannelCnt);
    CHECK(success);

    // If Resampler exists, delete it first
    if (mResampler != NULL) {
        delete mResampler;
        mResampler = NULL;
    }

    if (mSampleRate != mOutputSampleRate) {
        LOGV("Resampling required (%d != %d)", mSampleRate, mOutputSampleRate);
        mIsResamplingRequired = true;
        LOGV("Create resampler %d %d %d", mBitDepth, mChannelCnt, mOutputSampleRate);

        mResampler = AudioResampler::create(
                        mBitDepth, mChannelCnt, mOutputSampleRate, AudioResampler::DEFAULT);

        if (mResampler == NULL) {
            return NO_MEMORY;
        }
        LOGV("Set input rate %d", mSampleRate);
        mResampler->setSampleRate(mSampleRate);
        mResampler->setVolume(UNITY_GAIN, UNITY_GAIN);

    } else {
        LOGV("Resampling not required (%d = %d)", mSampleRate, mOutputSampleRate);
        mIsResamplingRequired = false;
        if (mChannelCnt != 2) {
            // we always make sure to provide stereo
            LOGV("Only Channel convertion required");
            mIsChannelConvertionRequired = true;
        }
    }

    return OK;

}

} //namespce android
