/*
** Copyright (c) 2012, The Linux Foundation. All rights reserved.
** Not a Contribution, Apache license notifications and license are retained
** for attribution purposes only.
**
** Copyright 2007, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "IDirectTrack"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <stdint.h>
#include <sys/types.h>

#include <binder/Parcel.h>

#include <media/IDirectTrack.h>

namespace android {

enum {
    START = IBinder::FIRST_CALL_TRANSACTION,
    STOP,
    FLUSH,
    MUTE,
    PAUSE,
    SET_VOLUME,
    WRITE,
    GET_TIMESTAMP
};

class BpDirectTrack : public BpInterface<IDirectTrack>
{
public:
    BpDirectTrack(const sp<IBinder>& impl)
        : BpInterface<IDirectTrack>(impl)
    {
    }

    virtual status_t start()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IDirectTrack::getInterfaceDescriptor());
        status_t status = remote()->transact(START, data, &reply);
        if (status == NO_ERROR) {
            status = reply.readInt32();
        } else {
            ALOGW("start() error: %s", strerror(-status));
        }
        return status;
    }

    virtual void stop()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IDirectTrack::getInterfaceDescriptor());
        remote()->transact(STOP, data, &reply);
    }

    virtual void flush()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IDirectTrack::getInterfaceDescriptor());
        remote()->transact(FLUSH, data, &reply);
    }

    virtual void mute(bool e)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IDirectTrack::getInterfaceDescriptor());
        data.writeInt32(e);
        remote()->transact(MUTE, data, &reply);
    }

    virtual void pause()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IDirectTrack::getInterfaceDescriptor());
        remote()->transact(PAUSE, data, &reply);
    }

    virtual void setVolume(float left, float right)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IDirectTrack::getInterfaceDescriptor());
        remote()->transact(SET_VOLUME, data, &reply);
    }

    virtual ssize_t write(const void* buffer, size_t bytes)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IDirectTrack::getInterfaceDescriptor());
        ssize_t bytesWritten = remote()->transact(WRITE, data, &reply);
        return bytesWritten;
    }

    virtual int64_t getTimeStamp() {
        Parcel data, reply;
        data.writeInterfaceToken(IDirectTrack::getInterfaceDescriptor());
        int64_t tstamp = remote()->transact(GET_TIMESTAMP, data, &reply);
        return tstamp;
    }
};

IMPLEMENT_META_INTERFACE(DirectTrack, "android.media.IDirectTrack");

// ----------------------------------------------------------------------

status_t BnDirectTrack::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch(code) {
        case START: {
            CHECK_INTERFACE(IDirectTrack, data, reply);
            reply->writeInt32(start());
            return NO_ERROR;
        } break;
        case STOP: {
            CHECK_INTERFACE(IDirectTrack, data, reply);
            stop();
            return NO_ERROR;
        } break;
        case FLUSH: {
            CHECK_INTERFACE(IDirectTrack, data, reply);
            flush();
            return NO_ERROR;
        } break;
        case MUTE: {
            CHECK_INTERFACE(IDirectTrack, data, reply);
            mute( data.readInt32() );
            return NO_ERROR;
        } break;
        case PAUSE: {
            CHECK_INTERFACE(IDirectTrack, data, reply);
            pause();
            return NO_ERROR;
        }
        case SET_VOLUME: {
            CHECK_INTERFACE(IDirectTrack, data, reply);
            float left = 1.0;
            float right = 1.0;
            setVolume(left, right);
            return NO_ERROR;
        }
        case WRITE: {
            CHECK_INTERFACE(IDirectTrack, data, reply);
            const void *buffer = (void *)data.readInt32();
            size_t bytes = data.readInt32();
            ssize_t bytesWritten = write(buffer, bytes);
            reply->writeInt32(bytesWritten);
            return NO_ERROR;
        }
        case GET_TIMESTAMP: {
            CHECK_INTERFACE(IDirectTrack, data, reply);
            int64_t time = getTimeStamp();
            reply->writeInt32(time);
            return NO_ERROR;
        }
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

}; // namespace android

