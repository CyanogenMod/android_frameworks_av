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
#define LOG_TAG "DummyAudioSource"
#include "utils/Log.h"

#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MetaData.h>


#include "DummyAudioSource.h"



/* Android includes*/
#include <utils/Log.h>
#include <memory.h>


/*---------------------*/
/*  DEBUG LEVEL SETUP  */
/*---------------------*/
#define LOG1 LOGE    /*ERRORS Logging*/
#define LOG2 LOGV    /*WARNING Logging*/
#define LOG3 //LOGV    /*COMMENTS Logging*/

/*---------------------*/
/*  CONSTANTS          */
/*---------------------*/


namespace android {


/*---------------------*/
/*  SEEK SOURCE        */
/*---------------------*/

//static
sp<DummyAudioSource> DummyAudioSource::Create(int32_t samplingRate,
                                              int32_t channelCount,
                                              int64_t frameDurationUs,
                                              int64_t audioDurationUs) {
    LOG2("DummyAudioSource::Create ");
    sp<DummyAudioSource> aSource = new DummyAudioSource (samplingRate,
                                                         channelCount,
                                                         frameDurationUs,
                                                         audioDurationUs);
    return aSource;
}


DummyAudioSource::DummyAudioSource (int32_t samplingRate,
                                    int32_t channelCount,
                                    int64_t frameDurationUs,
                                    int64_t audioDurationUs):
    mSamplingRate(samplingRate),
    mChannelCount(channelCount),
    mFrameDurationUs(frameDurationUs),
    mNumberOfSamplePerFrame(0),
    mAudioDurationUs(audioDurationUs),
    mTimeStampUs(0) ,

    mBufferGroup(NULL){
    LOG2("DummyAudioSource::DummyAudioSource constructor START");
    /* Do nothing here? */
    LOG2("DummyAudioSource::DummyAudioSource");
    LOG2("DummyAudioSource:: mSamplingRate = %d",samplingRate);
    LOG2("DummyAudioSource:: mChannelCount = %d",channelCount);
    LOG2("DummyAudioSource:: frameDurationUs = %lld",frameDurationUs);
    LOG2("DummyAudioSource:: mAudioDurationUs = %lld",mAudioDurationUs);

    LOG2("DummyAudioSource::DummyAudioSource constructor END");
}


DummyAudioSource::~DummyAudioSource () {
    /* Do nothing here? */
    LOG2("DummyAudioSource::~DummyAudioSource");
}

void DummyAudioSource::setDuration (int64_t audioDurationUs) {
    Mutex::Autolock autoLock(mLock);
    LOG2("SetDuration %lld", mAudioDurationUs);
    mAudioDurationUs += audioDurationUs;
    LOG2("SetDuration %lld", mAudioDurationUs);
}

status_t DummyAudioSource::start(MetaData *params) {
    status_t err = OK;
    LOG2("DummyAudioSource::start START");

    mTimeStampUs = 0;
    mNumberOfSamplePerFrame = (int32_t) ((1L * mSamplingRate * mFrameDurationUs)/1000000);
    mNumberOfSamplePerFrame = mNumberOfSamplePerFrame  * mChannelCount;

    mBufferGroup = new MediaBufferGroup;
    mBufferGroup->add_buffer(
            new MediaBuffer(mNumberOfSamplePerFrame * sizeof(int16_t)));

    LOG2("DummyAudioSource:: mNumberOfSamplePerFrame = %d",mNumberOfSamplePerFrame);
    LOG2("DummyAudioSource::start END");

    return err;
}


status_t DummyAudioSource::stop() {
    status_t err = OK;

    LOG2("DummyAudioSource::stop START");

    delete mBufferGroup;
    mBufferGroup = NULL;


    LOG2("DummyAudioSource::stop END");

    return err;
}


sp<MetaData> DummyAudioSource::getFormat() {
    LOG2("DummyAudioSource::getFormat");

    sp<MetaData> meta = new MetaData;

    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);
    meta->setInt32(kKeyChannelCount, mChannelCount);
    meta->setInt32(kKeySampleRate, mSamplingRate);
    meta->setInt64(kKeyDuration, mFrameDurationUs);

    meta->setCString(kKeyDecoderComponent, "DummyAudioSource");

    return meta;
}

status_t DummyAudioSource::read( MediaBuffer **out, const MediaSource::ReadOptions *options) {
    status_t err            = OK;
    //LOG2("DummyAudioSource::read START");
    MediaBuffer *buffer;
    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;

    if (options && options->getSeekTo(&seekTimeUs, &mode)) {
        CHECK(seekTimeUs >= 0);
        mTimeStampUs = seekTimeUs;
    }
    {
        Mutex::Autolock autoLock(mLock);
        if (mTimeStampUs >= mAudioDurationUs) {
            *out = NULL;
            LOGI("EOS reached");
            return ERROR_END_OF_STREAM;
        }
    }

    err = mBufferGroup->acquire_buffer(&buffer);
    if (err != OK) {
        return err;
    }

    memset((uint8_t *) buffer->data() + buffer->range_offset(),
            0, mNumberOfSamplePerFrame << 1);

    buffer->set_range(buffer->range_offset(), (mNumberOfSamplePerFrame << 1));

    buffer->meta_data()->setInt64(kKeyTime, mTimeStampUs);
    LOG2("DummyAudioSource::read  Buffer_offset  = %d,"
            "Buffer_Size = %d, mTimeStampUs = %lld",
             buffer->range_offset(),buffer->size(),mTimeStampUs);
    mTimeStampUs = mTimeStampUs + mFrameDurationUs;
    *out = buffer;
    return err;
}

}// namespace android
