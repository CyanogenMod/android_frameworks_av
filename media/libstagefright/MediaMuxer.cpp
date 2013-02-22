/*
 * Copyright 2013, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaMuxer"
#include <utils/Log.h>

#include <media/stagefright/MediaMuxer.h>

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaAdapter.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MPEG4Writer.h>
#include <media/stagefright/Utils.h>

namespace android {

MediaMuxer::MediaMuxer(const char *path, OutputFormat format)
    : mState(UNINITED) {
    if (format == OUTPUT_FORMAT_MPEG_4) {
        mWriter = new MPEG4Writer(path);
        mState = INITED;
    }
}

MediaMuxer::MediaMuxer(int fd, OutputFormat format)
    : mState(UNINITED) {
    if (format == OUTPUT_FORMAT_MPEG_4) {
        mWriter = new MPEG4Writer(fd);
        mState = INITED;
    }
}

MediaMuxer::~MediaMuxer() {
    Mutex::Autolock autoLock(mMuxerLock);

    // Clean up all the internal resources.
    mWriter.clear();
    mTrackList.clear();
}

ssize_t MediaMuxer::addTrack(const sp<AMessage> &format) {
    Mutex::Autolock autoLock(mMuxerLock);

    if (format.get() == NULL) {
        ALOGE("addTrack() get a null format");
        return -EINVAL;
    }

    if (mState != INITED) {
        ALOGE("addTrack() must be called after constructor and before start().");
        return INVALID_OPERATION;
    }

    sp<MetaData> meta = new MetaData;
    convertMessageToMetaData(format, meta);

    sp<MediaAdapter> newTrack = new MediaAdapter(meta);
    return mTrackList.add(newTrack);
}

status_t MediaMuxer::start() {
    Mutex::Autolock autoLock(mMuxerLock);

    if (mState == INITED) {
        mState = STARTED;
        for (size_t i = 0 ; i < mTrackList.size(); i++) {
            mWriter->addSource(mTrackList[i]);
        }
        return mWriter->start();
    } else {
        ALOGE("start() is called in invalid state %d", mState);
        return INVALID_OPERATION;
    }
}

status_t MediaMuxer::stop() {
    Mutex::Autolock autoLock(mMuxerLock);

    if (mState == STARTED) {
        mState = STOPPED;
        for (size_t i = 0; i < mTrackList.size(); i++) {
            mTrackList[i]->stop();
        }
        return mWriter->stop();
    } else {
        ALOGE("stop() is called in invalid state %d", mState);
        return INVALID_OPERATION;
    }
}

status_t MediaMuxer::writeSampleData(const sp<ABuffer> &buffer, size_t trackIndex,
                                     int64_t timeUs, uint32_t flags) {
    Mutex::Autolock autoLock(mMuxerLock);

    if (buffer.get() == NULL) {
        ALOGE("WriteSampleData() get an NULL buffer.");
        return -EINVAL;
    }

    if (mState != STARTED) {
        ALOGE("WriteSampleData() is called in invalid state %d", mState);
        return INVALID_OPERATION;
    }

    if (trackIndex >= mTrackList.size()) {
        ALOGE("WriteSampleData() get an invalid index %d", trackIndex);
        return -EINVAL;
    }

    MediaBuffer* mediaBuffer = new MediaBuffer(buffer);

    mediaBuffer->add_ref(); // Released in MediaAdapter::signalBufferReturned().
    mediaBuffer->set_range(buffer->offset(), buffer->size());

    sp<MetaData> metaData = mediaBuffer->meta_data();
    metaData->setInt64(kKeyTime, timeUs);
    // Just set the kKeyDecodingTime as the presentation time for now.
    metaData->setInt64(kKeyDecodingTime, timeUs);

    if (flags & SAMPLE_FLAG_SYNC) {
        metaData->setInt32(kKeyIsSyncFrame, true);
    }

    sp<MediaAdapter> currentTrack = mTrackList[trackIndex];
    // This pushBuffer will wait until the mediaBuffer is consumed.
    return currentTrack->pushBuffer(mediaBuffer);
}

}  // namespace android
