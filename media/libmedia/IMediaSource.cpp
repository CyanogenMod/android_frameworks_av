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

#include <inttypes.h>
#include <stdint.h>
#include <sys/types.h>

#include <binder/Parcel.h>
#include <media/IMediaSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>

namespace android {

enum {
    START = IBinder::FIRST_CALL_TRANSACTION,
    STOP,
    PAUSE,
    GETFORMAT,
    READ,
    READMULTIPLE,
    RELEASE_BUFFER
};

enum {
    NULL_BUFFER,
    SHARED_BUFFER,
    INLINE_BUFFER
};

class RemoteMediaBufferReleaser : public BBinder {
public:
    RemoteMediaBufferReleaser(MediaBuffer *buf, sp<BnMediaSource> owner) {
        mBuf = buf;
        mOwner = owner;
    }
    ~RemoteMediaBufferReleaser() {
        if (mBuf) {
            ALOGW("RemoteMediaBufferReleaser dtor called while still holding buffer");
            mBuf->release();
        }
    }
    virtual status_t    onTransact( uint32_t code,
                                    const Parcel& data,
                                    Parcel* reply,
                                    uint32_t flags = 0) {
        if (code == RELEASE_BUFFER) {
            mBuf->release();
            mBuf = NULL;
            return OK;
        } else {
            return BBinder::onTransact(code, data, reply, flags);
        }
    }
private:
    MediaBuffer *mBuf;
    // Keep a ref to ensure MediaBuffer is released before the owner, i.e., BnMediaSource,
    // because BnMediaSource needs to delete MediaBufferGroup in its dtor and
    // MediaBufferGroup dtor requires all MediaBuffer's have 0 ref count.
    sp<BnMediaSource> mOwner;
};


class RemoteMediaBufferWrapper : public MediaBuffer {
public:
    RemoteMediaBufferWrapper(sp<IMemory> mem, sp<IBinder> source);
protected:
    virtual ~RemoteMediaBufferWrapper();
private:
    sp<IMemory> mMemory;
    sp<IBinder> mRemoteSource;
};

RemoteMediaBufferWrapper::RemoteMediaBufferWrapper(sp<IMemory> mem, sp<IBinder> source)
: MediaBuffer(mem->pointer(), mem->size()) {
    mMemory = mem;
    mRemoteSource = source;
}

RemoteMediaBufferWrapper::~RemoteMediaBufferWrapper() {
    mMemory.clear();
    // Explicitly ask the remote side to release the buffer. We could also just clear
    // mRemoteSource, but that doesn't immediately release the reference on the remote side.
    Parcel data, reply;
    mRemoteSource->transact(RELEASE_BUFFER, data, &reply);
    mRemoteSource.clear();
}

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
        ret = reply.readInt32();
        int32_t buftype = reply.readInt32();
        if (buftype == SHARED_BUFFER) {
            sp<IBinder> remote = reply.readStrongBinder();
            sp<IBinder> binder = reply.readStrongBinder();
            sp<IMemory> mem = interface_cast<IMemory>(binder);
            if (mem == NULL) {
                ALOGE("received NULL IMemory for shared buffer");
            }
            size_t offset = reply.readInt32();
            size_t length = reply.readInt32();
            MediaBuffer *buf = new RemoteMediaBufferWrapper(mem, remote);
            buf->set_range(offset, length);
            buf->meta_data()->updateFromParcel(reply);
            *buffer = buf;
        } else if (buftype == NULL_BUFFER) {
            ALOGV("got status %d and NULL buffer", ret);
            *buffer = NULL;
        } else {
            int32_t len = reply.readInt32();
            ALOGV("got status %d and len %d", ret, len);
            *buffer = new MediaBuffer(len);
            reply.read((*buffer)->data(), len);
            (*buffer)->meta_data()->updateFromParcel(reply);
        }
        return ret;
    }

    virtual status_t readMultiple(Vector<MediaBuffer *> *buffers, uint32_t maxNumBuffers) {
        ALOGV("readMultiple");
        if (buffers == NULL || !buffers->isEmpty()) {
            return BAD_VALUE;
        }
        Parcel data, reply;
        data.writeInterfaceToken(BpMediaSource::getInterfaceDescriptor());
        data.writeUint32(maxNumBuffers);
        status_t ret = remote()->transact(READMULTIPLE, data, &reply);
        if (ret != NO_ERROR) {
            return ret;
        }
        // wrap the returned data in a vector of MediaBuffers
        int32_t bufCount = 0;
        while (1) {
            if (reply.readInt32() == 0) {
                break;
            }
            int32_t len = reply.readInt32();
            ALOGV("got len %d", len);
            MediaBuffer *buf = new MediaBuffer(len);
            reply.read(buf->data(), len);
            buf->meta_data()->updateFromParcel(reply);
            buffers->push_back(buf);
            ++bufCount;
        }
        ret = reply.readInt32();
        ALOGV("got status %d, bufCount %d", ret, bufCount);
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

BnMediaSource::BnMediaSource()
    : mGroup(NULL) {
}

BnMediaSource::~BnMediaSource() {
    delete mGroup;
    mGroup = NULL;
}

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

            reply->writeInt32(ret);
            if (buf != NULL) {
                size_t usedSize = buf->range_length();
                // even if we're using shared memory, we might not want to use it, since for small
                // sizes it's faster to copy data through the Binder transaction
                // On the other hand, if the data size is large enough, it's better to use shared
                // memory. When data is too large, binder can't handle it.
                if (usedSize >= MediaBuffer::kSharedMemThreshold) {
                    ALOGV("use shared memory: %zu", usedSize);

                    MediaBuffer *transferBuf = buf;
                    size_t offset = buf->range_offset();
                    if (transferBuf->mMemory == NULL) {
                        if (mGroup == NULL) {
                            mGroup = new MediaBufferGroup;
                            size_t allocateSize = usedSize;
                            if (usedSize < SIZE_MAX / 3) {
                                allocateSize = usedSize * 3 / 2;
                            }
                            mGroup->add_buffer(new MediaBuffer(allocateSize));
                        }

                        MediaBuffer *newBuf = NULL;
                        ret = mGroup->acquire_buffer(
                                &newBuf, false /* nonBlocking */, usedSize);
                        if (ret != OK || newBuf == NULL || newBuf->mMemory == NULL) {
                            ALOGW("failed to acquire shared memory, ret %d", ret);
                            buf->release();
                            if (newBuf != NULL) {
                                newBuf->release();
                            }
                            reply->writeInt32(NULL_BUFFER);
                            return NO_ERROR;
                        }
                        transferBuf = newBuf;
                        memcpy(transferBuf->data(), (uint8_t*)buf->data() + buf->range_offset(),
                                buf->range_length());
                        offset = 0;
                    }

                    reply->writeInt32(SHARED_BUFFER);
                    RemoteMediaBufferReleaser *wrapper =
                        new RemoteMediaBufferReleaser(transferBuf, this);
                    reply->writeStrongBinder(wrapper);
                    reply->writeStrongBinder(IInterface::asBinder(transferBuf->mMemory));
                    reply->writeInt32(offset);
                    reply->writeInt32(usedSize);
                    buf->meta_data()->writeToParcel(*reply);
                    if (buf->mMemory == NULL) {
                        buf->release();
                    }
                } else {
                    // buffer is small: copy it
                    if (buf->mMemory != NULL) {
                        ALOGV("%zu shared mem available, but only %zu used", buf->mMemory->size(), buf->range_length());
                    }
                    reply->writeInt32(INLINE_BUFFER);
                    reply->writeByteArray(buf->range_length(), (uint8_t*)buf->data() + buf->range_offset());
                    buf->meta_data()->writeToParcel(*reply);
                    buf->release();
                }
            } else {
                ALOGV("ret %d, buf %p", ret, buf);
                reply->writeInt32(NULL_BUFFER);
            }
            return NO_ERROR;
        }
        case READMULTIPLE: {
            ALOGV("readmultiple");
            CHECK_INTERFACE(IMediaSource, data, reply);
            uint32_t maxNumBuffers;
            data.readUint32(&maxNumBuffers);
            status_t ret = NO_ERROR;
            uint32_t bufferCount = 0;
            if (maxNumBuffers > kMaxNumReadMultiple) {
                maxNumBuffers = kMaxNumReadMultiple;
            }
            while (bufferCount < maxNumBuffers) {
                if (reply->dataSize() >= MediaBuffer::kSharedMemThreshold) {
                    break;
                }

                MediaBuffer *buf = NULL;
                ret = read(&buf, NULL);
                if (ret != NO_ERROR || buf == NULL) {
                    break;
                }
                ++bufferCount;
                reply->writeInt32(1);  // indicate one more MediaBuffer.
                reply->writeByteArray(
                        buf->range_length(), (uint8_t*)buf->data() + buf->range_offset());
                buf->meta_data()->writeToParcel(*reply);
                buf->release();
            }
            reply->writeInt32(0);  // indicate no more MediaBuffer.
            reply->writeInt32(ret);
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

