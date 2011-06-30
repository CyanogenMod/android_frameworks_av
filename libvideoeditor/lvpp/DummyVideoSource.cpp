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
#define LOG_TAG "DummyVideoSource"
#include "utils/Log.h"

#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MetaData.h>

#include "DummyVideoSource.h"

/* Android includes*/
#include <utils/Log.h>
#include <memory.h>


#define LOG1 LOGE    /*ERRORS Logging*/
#define LOG2 LOGV    /*WARNING Logging*/
#define LOG3 //LOGV    /*COMMENTS Logging*/


namespace android {


sp<DummyVideoSource> DummyVideoSource::Create (
            uint32_t width, uint32_t height,
            uint64_t clipDuration, const char *imageUri) {
    LOG2("DummyVideoSource::Create ");
    sp<DummyVideoSource> vSource = new DummyVideoSource (
                         width, height, clipDuration, imageUri);
    return vSource;
}


DummyVideoSource::DummyVideoSource (
            uint32_t width, uint32_t height,
            uint64_t clipDuration, const char *imageUri) {

    LOG2("DummyVideoSource::DummyVideoSource constructor START");
    mFrameWidth = width;
    mFrameHeight = height;
    mImageClipDuration = clipDuration;
    mUri = imageUri;
    mImageBuffer = NULL;

    LOG2("DummyVideoSource::DummyVideoSource constructor END");
}


DummyVideoSource::~DummyVideoSource () {
    /* Do nothing here? */
    LOG2("DummyVideoSource::~DummyVideoSource");
}



status_t DummyVideoSource::start(MetaData *params) {
    status_t err = OK;
    LOG2("DummyVideoSource::start START, %s", mUri);
    //get the frame buffer from the rgb file and store into a MediaBuffer
    err = LvGetImageThumbNail((const char *)mUri,
                          mFrameHeight , mFrameWidth ,
                          (M4OSA_Void **)&mImageBuffer);

    mIsFirstImageFrame = true;
    mImageSeekTime = 0;
    mImagePlayStartTime = 0;
    mFrameTimeUs = 0;
    LOG2("DummyVideoSource::start END");

    return err;
}


status_t DummyVideoSource::stop() {
    status_t err = OK;

    LOG2("DummyVideoSource::stop START");
    if (mImageBuffer != NULL) {
        free(mImageBuffer);
        mImageBuffer = NULL;
    }
    LOG2("DummyVideoSource::stop END");

    return err;
}


sp<MetaData> DummyVideoSource::getFormat() {
    LOG2("DummyVideoSource::getFormat");

    sp<MetaData> meta = new MetaData;

    meta->setInt32(kKeyColorFormat, OMX_COLOR_FormatYUV420Planar);
    meta->setInt32(kKeyWidth, mFrameWidth);
    meta->setInt32(kKeyHeight, mFrameHeight);
    meta->setInt64(kKeyDuration, mImageClipDuration);
    meta->setCString(kKeyDecoderComponent, "DummyVideoSource");

    return meta;
}

status_t DummyVideoSource::read(
                        MediaBuffer **out,
                        const MediaSource::ReadOptions *options) {
    status_t err = OK;
    MediaBuffer *buffer;
    LOG2("DummyVideoSource::read START");

    bool seeking = false;
    int64_t seekTimeUs;
    ReadOptions::SeekMode seekMode;

    if (options && options->getSeekTo(&seekTimeUs, &seekMode)) {
        seeking = true;
        mImageSeekTime = seekTimeUs;
        M4OSA_clockGetTime(&mImagePlayStartTime, 1000); //1000 time scale for time in ms
    }

    if ((mImageSeekTime == mImageClipDuration) || (mFrameTimeUs == (int64_t)mImageClipDuration)) {
        LOG2("DummyVideoSource::read() End of stream reached; return NULL buffer");
        *out = NULL;
        return ERROR_END_OF_STREAM;
    }

    buffer = new MediaBuffer(mImageBuffer, (mFrameWidth*mFrameHeight*1.5));

    //set timestamp of buffer
    if (mIsFirstImageFrame) {
        M4OSA_clockGetTime(&mImagePlayStartTime, 1000); //1000 time scale for time in ms
        mFrameTimeUs =  (mImageSeekTime + 1);
        LOG2("DummyVideoSource::read() jpg 1st frame timeUs = %lld, begin cut time = %ld", mFrameTimeUs, mImageSeekTime);
        mIsFirstImageFrame = false;
    } else {
        M4OSA_Time  currentTimeMs;
        M4OSA_clockGetTime(&currentTimeMs, 1000);

        mFrameTimeUs = mImageSeekTime + (currentTimeMs - mImagePlayStartTime)*1000;
        LOG2("DummyVideoSource::read() jpg frame timeUs = %lld", mFrameTimeUs);
    }
    buffer->meta_data()->setInt64(kKeyTime, mFrameTimeUs);
    buffer->set_range(buffer->range_offset(), mFrameWidth*mFrameHeight*1.5);
    *out = buffer;
    return err;
}

}// namespace android
