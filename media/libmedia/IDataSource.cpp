/*
 * Copyright 2015 The Android Open Source Project
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
#define LOG_TAG "IDataSource"
#include <utils/Log.h>
#include <utils/Timers.h>

#include <media/IDataSource.h>

#include <binder/IMemory.h>
#include <binder/Parcel.h>
#include <media/stagefright/foundation/ADebug.h>

namespace android {

enum {
    GET_IMEMORY = IBinder::FIRST_CALL_TRANSACTION,
    READ_AT,
    GET_SIZE,
    CLOSE,
    GET_FLAGS,
    TO_STRING,
};

struct BpDataSource : public BpInterface<IDataSource> {
    BpDataSource(const sp<IBinder>& impl) : BpInterface<IDataSource>(impl) {}

    virtual sp<IMemory> getIMemory() {
        Parcel data, reply;
        data.writeInterfaceToken(IDataSource::getInterfaceDescriptor());
        remote()->transact(GET_IMEMORY, data, &reply);
        sp<IBinder> binder = reply.readStrongBinder();
        return interface_cast<IMemory>(binder);
    }

    virtual ssize_t readAt(off64_t offset, size_t size) {
        Parcel data, reply;
        data.writeInterfaceToken(IDataSource::getInterfaceDescriptor());
        data.writeInt64(offset);
        data.writeInt64(size);
        remote()->transact(READ_AT, data, &reply);
        return reply.readInt64();
    }

    virtual status_t getSize(off64_t* size) {
        Parcel data, reply;
        data.writeInterfaceToken(IDataSource::getInterfaceDescriptor());
        remote()->transact(GET_SIZE, data, &reply);
        status_t err = reply.readInt32();
        *size = reply.readInt64();
        return err;
    }

    virtual void close() {
        Parcel data, reply;
        data.writeInterfaceToken(IDataSource::getInterfaceDescriptor());
        remote()->transact(CLOSE, data, &reply);
    }

    virtual uint32_t getFlags() {
        Parcel data, reply;
        data.writeInterfaceToken(IDataSource::getInterfaceDescriptor());
        remote()->transact(GET_FLAGS, data, &reply);
        return reply.readUint32();
    }

    virtual String8 toString() {
        Parcel data, reply;
        data.writeInterfaceToken(IDataSource::getInterfaceDescriptor());
        remote()->transact(TO_STRING, data, &reply);
        return reply.readString8();
    }
};

IMPLEMENT_META_INTERFACE(DataSource, "android.media.IDataSource");

status_t BnDataSource::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) {
    switch (code) {
        case GET_IMEMORY: {
            CHECK_INTERFACE(IDataSource, data, reply);
            reply->writeStrongBinder(IInterface::asBinder(getIMemory()));
            return NO_ERROR;
        } break;
        case READ_AT: {
            CHECK_INTERFACE(IDataSource, data, reply);
            off64_t offset = (off64_t) data.readInt64();
            size_t size = (size_t) data.readInt64();
            reply->writeInt64(readAt(offset, size));
            return NO_ERROR;
        } break;
        case GET_SIZE: {
            CHECK_INTERFACE(IDataSource, data, reply);
            off64_t size;
            status_t err = getSize(&size);
            reply->writeInt32(err);
            reply->writeInt64(size);
            return NO_ERROR;
        } break;
        case CLOSE: {
            CHECK_INTERFACE(IDataSource, data, reply);
            close();
            return NO_ERROR;
        } break;
        case GET_FLAGS: {
            CHECK_INTERFACE(IDataSource, data, reply);
            reply->writeUint32(getFlags());
            return NO_ERROR;
        } break;
        case TO_STRING: {
            CHECK_INTERFACE(IDataSource, data, reply);
            reply->writeString8(toString());
            return NO_ERROR;
        } break;

        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

}  // namespace android
