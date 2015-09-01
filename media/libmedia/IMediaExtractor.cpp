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

#define LOG_NDEBUG 0
#define LOG_TAG "BpMediaExtractor"
#include <utils/Log.h>

#include <stdint.h>
#include <sys/types.h>

#include <binder/Parcel.h>
#include <media/IMediaExtractor.h>
#include <media/stagefright/MetaData.h>

namespace android {

enum {
    COUNTTRACKS = IBinder::FIRST_CALL_TRANSACTION,
    GETTRACK,
    GETTRACKMETADATA,
    GETMETADATA,
    FLAGS,
    SETDRMFLAG,
    GETDRMFLAG,
    GETDRMTRACKINFO,
    SETUID,
    NAME
};

class BpMediaExtractor : public BpInterface<IMediaExtractor> {
public:
    BpMediaExtractor(const sp<IBinder>& impl)
        : BpInterface<IMediaExtractor>(impl)
    {
    }

    virtual size_t countTracks() {
        ALOGV("countTracks");
        Parcel data, reply;
        data.writeInterfaceToken(BpMediaExtractor::getInterfaceDescriptor());
        status_t ret = remote()->transact(COUNTTRACKS, data, &reply);
        size_t numTracks = 0;
        if (ret == NO_ERROR) {
            numTracks = reply.readUint32();
        }
        return numTracks;
    }
    virtual sp<IMediaSource> getTrack(size_t index) {
        ALOGV("getTrack(%zu)", index);
        Parcel data, reply;
        data.writeInterfaceToken(BpMediaExtractor::getInterfaceDescriptor());
        data.writeUint32(index);
        status_t ret = remote()->transact(GETTRACK, data, &reply);
        if (ret == NO_ERROR) {
            return interface_cast<IMediaSource>(reply.readStrongBinder());
        }
        return NULL;
    }

    virtual sp<MetaData> getTrackMetaData(
            size_t index, uint32_t flags) {
        ALOGV("getTrackMetaData(%zu, %u)", index, flags);
        Parcel data, reply;
        data.writeInterfaceToken(BpMediaExtractor::getInterfaceDescriptor());
        data.writeUint32(index);
        data.writeUint32(flags);
        status_t ret = remote()->transact(GETTRACKMETADATA, data, &reply);
        if (ret == NO_ERROR) {
            return MetaData::createFromParcel(reply);
        }
        return NULL;
    }

    virtual sp<MetaData> getMetaData() {
        ALOGV("getMetaData");
        Parcel data, reply;
        data.writeInterfaceToken(BpMediaExtractor::getInterfaceDescriptor());
        status_t ret = remote()->transact(GETMETADATA, data, &reply);
        if (ret == NO_ERROR) {
            return MetaData::createFromParcel(reply);
        }
        return NULL;
    }

    virtual uint32_t flags() const {
        ALOGV("flags NOT IMPLEMENTED");
        return 0;
    }

    virtual void setDrmFlag(bool flag __unused) {
        ALOGV("setDrmFlag NOT IMPLEMENTED");
    }
    virtual bool getDrmFlag() {
        ALOGV("getDrmFlag NOT IMPLEMENTED");
       return false;
    }
    virtual char* getDrmTrackInfo(size_t trackID __unused, int *len __unused) {
        ALOGV("getDrmTrackInfo NOT IMPLEMENTED");
        return NULL;
    }
    virtual void setUID(uid_t uid __unused) {
        ALOGV("setUID NOT IMPLEMENTED");
    }

    virtual const char * name() {
        ALOGV("name NOT IMPLEMENTED");
        return NULL;
    }
};

IMPLEMENT_META_INTERFACE(MediaExtractor, "android.media.IMediaExtractor");

#undef LOG_TAG
#define LOG_TAG "BnMediaExtractor"

status_t BnMediaExtractor::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch (code) {
        case COUNTTRACKS: {
            ALOGV("countTracks");
            CHECK_INTERFACE(IMediaExtractor, data, reply);
            size_t numTracks = countTracks();
            if (numTracks > INT32_MAX) {
                numTracks = 0;
            }
            reply->writeUint32(uint32_t(numTracks));
            return NO_ERROR;
        }
        case GETTRACK: {
            ALOGV("getTrack()");
            CHECK_INTERFACE(IMediaExtractor, data, reply);
            uint32_t idx;
            if (data.readUint32(&idx) == NO_ERROR) {
                return reply->writeStrongBinder(IInterface::asBinder(getTrack((size_t(idx)))));
            }
            return UNKNOWN_ERROR;
        }
        case GETTRACKMETADATA: {
            ALOGV("getTrackMetaData");
            CHECK_INTERFACE(IMediaExtractor, data, reply);
            uint32_t idx;
            uint32_t flags;
            if (data.readUint32(&idx) == NO_ERROR &&
                    data.readUint32(&flags) == NO_ERROR) {
                sp<MetaData> meta = getTrackMetaData(idx, flags);
                meta->writeToParcel(*reply);
                return NO_ERROR;
            }
            return UNKNOWN_ERROR;
        }
        case GETMETADATA: {
            ALOGV("getMetaData");
            CHECK_INTERFACE(IMediaExtractor, data, reply);
            sp<MetaData> meta = getMetaData();
            if (meta != NULL) {
                meta->writeToParcel(*reply);
                return NO_ERROR;
            }
            return UNKNOWN_ERROR;
        }
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}


}  // namespace android

