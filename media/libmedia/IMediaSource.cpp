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

//#define LOG_NDEBUG 0
#define LOG_TAG "BpMediaSource"
#include <utils/Log.h>

#include <stdint.h>
#include <sys/types.h>

#include <binder/Parcel.h>
#include <media/IMediaSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>

namespace android {

enum {
    START = IBinder::FIRST_CALL_TRANSACTION,
    STOP,
    PAUSE,
    GETFORMAT,
    READ
};

class BpMediaSource : public BpInterface<IMediaSource> {
public:
    BpMediaSource(const sp<IBinder>& impl)
        : BpInterface<IMediaSource>(impl)
    {
    }

    virtual status_t start(MetaData *params) {
        ALOGV("start");
        Parcel data, reply;
        data.writeInterfaceToken(BpMediaSource::getInterfaceDescriptor());
        if (params) {
            params->writeToParcel(data);
        }
        status_t ret = remote()->transact(START, data, &reply);
        if (ret == NO_ERROR && params) {
            ALOGW("ignoring potentially modified MetaData from start");
            ALOGW("input:");
            params->dumpToLog();
            sp<MetaData> meta = MetaData::createFromParcel(reply);
            ALOGW("output:");
            meta->dumpToLog();
        }
        return ret;
    }

    virtual status_t stop() {
        ALOGV("stop");
        Parcel data, reply;
        data.writeInterfaceToken(BpMediaSource::getInterfaceDescriptor());
        return remote()->transact(STOP, data, &reply);
    }

    virtual sp<MetaData> getFormat() {
        ALOGV("getFormat");
        Parcel data, reply;
        data.writeInterfaceToken(BpMediaSource::getInterfaceDescriptor());
        status_t ret = remote()->transact(GETFORMAT, data, &reply);
        if (ret == NO_ERROR) {
            mMetaData = MetaData::createFromParcel(reply);
            return mMetaData;
        }
        return NULL;
    }

    virtual status_t read(MediaBuffer **buffer, const ReadOptions *options) {
        ALOGV("read");
        Parcel data, reply;
        data.writeInterfaceToken(BpMediaSource::getInterfaceDescriptor());
        if (options) {
            data.writeByteArray(sizeof(*options), (uint8_t*) options);
        }
        status_t ret = remote()->transact(READ, data, &reply);
        if (ret != NO_ERROR) {
            return ret;
        }
        // wrap the returned data in a MediaBuffer
        // XXX use a group, and use shared memory for transfer
        ret = reply.readInt32();
        int32_t len = reply.readInt32();
        if (len < 0) {
            ALOGV("got status %d and len %d, returning NULL buffer", ret, len);
            *buffer = NULL;
        } else {
            ALOGV("got status %d and len %d", ret, len);
            *buffer = new MediaBuffer(len);
            reply.read((*buffer)->data(), len);
            (*buffer)->meta_data()->updateFromParcel(reply);
        }
        return ret;
    }

    virtual status_t pause() {
        ALOGV("pause");
        Parcel data, reply;
        data.writeInterfaceToken(BpMediaSource::getInterfaceDescriptor());
        return remote()->transact(PAUSE, data, &reply);
    }

    virtual status_t setBuffers(const Vector<MediaBuffer *> & buffers __unused) {
        ALOGV("setBuffers NOT IMPLEMENTED");
        return ERROR_UNSUPPORTED; // default
    }

private:
    // NuPlayer passes pointers-to-metadata around, so we use this to keep the metadata alive
    // XXX: could we use this for caching, or does metadata change on the fly?
    sp<MetaData> mMetaData;

};

IMPLEMENT_META_INTERFACE(MediaSource, "android.media.IMediaSource");

#undef LOG_TAG
#define LOG_TAG "BnMediaSource"

status_t BnMediaSource::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch (code) {
        case START: {
            ALOGV("start");
            CHECK_INTERFACE(IMediaSource, data, reply);
            sp<MetaData> meta;
            if (data.dataAvail()) {
                meta = MetaData::createFromParcel(data);
            }
            status_t ret = start(meta.get());
            if (ret == NO_ERROR && meta != NULL) {
                meta->writeToParcel(*reply);
            }
            return ret;
        }
        case STOP: {
            ALOGV("stop");
            CHECK_INTERFACE(IMediaSource, data, reply);
            return stop();
        }
        case PAUSE: {
            ALOGV("pause");
            CHECK_INTERFACE(IMediaSource, data, reply);
            return pause();
        }
        case GETFORMAT: {
            ALOGV("getFormat");
            CHECK_INTERFACE(IMediaSource, data, reply);
            sp<MetaData> meta = getFormat();
            if (meta != NULL) {
                meta->writeToParcel(*reply);
                return NO_ERROR;
            }
            return UNKNOWN_ERROR;
        }
        case READ: {
            ALOGV("read");
            CHECK_INTERFACE(IMediaSource, data, reply);
            status_t ret;
            MediaBuffer *buf = NULL;
            ReadOptions opts;
            uint32_t len;
            if (data.readUint32(&len) == NO_ERROR &&
                    len == sizeof(opts) && data.read((void*)&opts, len) == NO_ERROR) {
                ret = read(&buf, &opts);
            } else {
                ret = read(&buf, NULL);
            }
            // return data inside binder for now
            // XXX return data using shared memory
            reply->writeInt32(ret);
            if (buf != NULL) {
                ALOGV("ret %d, buflen %zu", ret, buf->range_length());
                reply->writeByteArray(buf->range_length(), (uint8_t*)buf->data() + buf->range_offset());
                buf->meta_data()->writeToParcel(*reply);
                buf->release();
            } else {
                ALOGV("ret %d, buf %p", ret, buf);
                reply->writeInt32(-1);
            }
            return NO_ERROR;
        }
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

////////////////////////////////////////////////////////////////////////////////

IMediaSource::ReadOptions::ReadOptions() {
    reset();
}

void IMediaSource::ReadOptions::reset() {
    mOptions = 0;
    mSeekTimeUs = 0;
    mLatenessUs = 0;
    mNonBlocking = false;
}

void IMediaSource::ReadOptions::setNonBlocking() {
    mNonBlocking = true;
}

void IMediaSource::ReadOptions::clearNonBlocking() {
    mNonBlocking = false;
}

bool IMediaSource::ReadOptions::getNonBlocking() const {
    return mNonBlocking;
}

void IMediaSource::ReadOptions::setSeekTo(int64_t time_us, SeekMode mode) {
    mOptions |= kSeekTo_Option;
    mSeekTimeUs = time_us;
    mSeekMode = mode;
}

void IMediaSource::ReadOptions::clearSeekTo() {
    mOptions &= ~kSeekTo_Option;
    mSeekTimeUs = 0;
    mSeekMode = SEEK_CLOSEST_SYNC;
}

bool IMediaSource::ReadOptions::getSeekTo(
        int64_t *time_us, SeekMode *mode) const {
    *time_us = mSeekTimeUs;
    *mode = mSeekMode;
    return (mOptions & kSeekTo_Option) != 0;
}

void IMediaSource::ReadOptions::setLateBy(int64_t lateness_us) {
    mLatenessUs = lateness_us;
}

int64_t IMediaSource::ReadOptions::getLateBy() const {
    return mLatenessUs;
}


}  // namespace android

