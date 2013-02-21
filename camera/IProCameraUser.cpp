/*
**
** Copyright 2013, The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "IProCameraUser"
#include <utils/Log.h>
#include <stdint.h>
#include <sys/types.h>
#include <binder/Parcel.h>
#include <camera/IProCameraUser.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>
#include <system/camera_metadata.h>

namespace android {

typedef Parcel::WritableBlob WritableBlob;
typedef Parcel::ReadableBlob ReadableBlob;

enum {
    DISCONNECT = IBinder::FIRST_CALL_TRANSACTION,
    CONNECT,
    EXCLUSIVE_TRY_LOCK,
    EXCLUSIVE_LOCK,
    EXCLUSIVE_UNLOCK,
    HAS_EXCLUSIVE_LOCK,
    SUBMIT_REQUEST,
    CANCEL_REQUEST,
    REQUEST_STREAM,
    CANCEL_STREAM,
    CREATE_STREAM,
    CREATE_DEFAULT_REQUEST,
};

/**
  * Caller becomes the owner of the new metadata
  * 'const Parcel' doesnt prevent us from calling the read functions.
  *  which is interesting since it changes the internal state
  */
void readMetadata(const Parcel& data, camera_metadata_t** out) {
    camera_metadata_t* metadata;

    // arg0 = metadataSize (int32)
    size_t metadataSize = static_cast<size_t>(data.readInt32());

    if (metadataSize == 0) {
        if (out) {
            *out = NULL;
        }
        return;
    }

    // NOTE: this doesn't make sense to me. shouldnt the blob
    // know how big it is? why do we have to specify the size
    // to Parcel::readBlob ?

    ReadableBlob blob;
    // arg1 = metadata (blob)
    {
        data.readBlob(metadataSize, &blob);
        const camera_metadata_t* tmp =
                       reinterpret_cast<const camera_metadata_t*>(blob.data());
        size_t entry_capacity = get_camera_metadata_entry_capacity(tmp);
        size_t data_capacity = get_camera_metadata_data_capacity(tmp);

        metadata = allocate_camera_metadata(entry_capacity, data_capacity);
        copy_camera_metadata(metadata, metadataSize, tmp);
    }
    blob.release();

    if (out) {
        *out = metadata;
    } else {
        free_camera_metadata(metadata);
    }
}

/**
  * Caller retains ownership of metadata
  * - Write 2 (int32 + blob) args in the current position
  */
void writeMetadata(Parcel& data, camera_metadata_t* metadata) {
    // arg0 = metadataSize (int32)
    size_t metadataSize;

    if (metadata == NULL) {
        data.writeInt32(0);
        return;
    }

    metadataSize = get_camera_metadata_compact_size(metadata);
    data.writeInt32(static_cast<int32_t>(metadataSize));

    // arg1 = metadata (blob)
    WritableBlob blob;
    {
        data.writeBlob(metadataSize, &blob);
        copy_camera_metadata(blob.data(), metadataSize, metadata);
    }
    blob.release();
}

class BpProCameraUser: public BpInterface<IProCameraUser>
{
public:
    BpProCameraUser(const sp<IBinder>& impl)
        : BpInterface<IProCameraUser>(impl)
    {
    }

    // disconnect from camera service
    void disconnect()
    {
        ALOGV("disconnect");
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        remote()->transact(DISCONNECT, data, &reply);
    }

    virtual status_t connect(const sp<IProCameraCallbacks>& cameraClient)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        data.writeStrongBinder(cameraClient->asBinder());
        remote()->transact(CONNECT, data, &reply);
        return reply.readInt32();
    }

    /* Shared ProCameraUser */

    virtual status_t exclusiveTryLock()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        remote()->transact(EXCLUSIVE_TRY_LOCK, data, &reply);
        return reply.readInt32();
    }
    virtual status_t exclusiveLock()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        remote()->transact(EXCLUSIVE_LOCK, data, &reply);
        return reply.readInt32();
    }

    virtual status_t exclusiveUnlock()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        remote()->transact(EXCLUSIVE_UNLOCK, data, &reply);
        return reply.readInt32();
    }

    virtual bool hasExclusiveLock()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        remote()->transact(HAS_EXCLUSIVE_LOCK, data, &reply);
        return !!reply.readInt32();
    }

    virtual int submitRequest(camera_metadata_t* metadata, bool streaming)
    {

        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());

        // arg0+arg1
        writeMetadata(data, metadata);

        // arg2 = streaming (bool)
        data.writeInt32(streaming);

        remote()->transact(SUBMIT_REQUEST, data, &reply);
        return reply.readInt32();
    }

    virtual status_t cancelRequest(int requestId)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        data.writeInt32(requestId);

        remote()->transact(CANCEL_REQUEST, data, &reply);
        return reply.readInt32();
    }

    virtual status_t requestStream(int streamId)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        data.writeInt32(streamId);

        remote()->transact(REQUEST_STREAM, data, &reply);
        return reply.readInt32();
    }
    virtual status_t cancelStream(int streamId)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        data.writeInt32(streamId);

        remote()->transact(CANCEL_STREAM, data, &reply);
        return reply.readInt32();
    }

    virtual status_t createStream(int width, int height, int format,
                          const sp<Surface>& surface,
                          /*out*/
                          int* streamId)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        data.writeInt32(width);
        data.writeInt32(height);
        data.writeInt32(format);

        Surface::writeToParcel(surface, &data);
        remote()->transact(CREATE_STREAM, data, &reply);

        int sId = reply.readInt32();
        if (streamId) {
            *streamId = sId;
        }
        return reply.readInt32();
    }

    // Create a request object from a template.
    virtual status_t createDefaultRequest(int templateId,
                                 /*out*/
                                  camera_metadata** request)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        data.writeInt32(templateId);
        remote()->transact(CREATE_DEFAULT_REQUEST, data, &reply);
        readMetadata(reply, /*out*/request);
        return reply.readInt32();
    }


private:


};

IMPLEMENT_META_INTERFACE(ProCameraUser, "android.hardware.IProCameraUser");

// ----------------------------------------------------------------------

status_t BnProCameraUser::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch(code) {
        case DISCONNECT: {
            ALOGV("DISCONNECT");
            CHECK_INTERFACE(IProCameraUser, data, reply);
            disconnect();
            return NO_ERROR;
        } break;
        case CONNECT: {
            CHECK_INTERFACE(IProCameraUser, data, reply);
            sp<IProCameraCallbacks> cameraClient =
                   interface_cast<IProCameraCallbacks>(data.readStrongBinder());
            reply->writeInt32(connect(cameraClient));
            return NO_ERROR;
        } break;

        /* Shared ProCameraUser */
        case EXCLUSIVE_TRY_LOCK: {
            CHECK_INTERFACE(IProCameraUser, data, reply);
            reply->writeInt32(exclusiveTryLock());
            return NO_ERROR;
        } break;
        case EXCLUSIVE_LOCK: {
            CHECK_INTERFACE(IProCameraUser, data, reply);
            reply->writeInt32(exclusiveLock());
            return NO_ERROR;
        } break;
        case EXCLUSIVE_UNLOCK: {
            CHECK_INTERFACE(IProCameraUser, data, reply);
            reply->writeInt32(exclusiveUnlock());
            return NO_ERROR;
        } break;
        case HAS_EXCLUSIVE_LOCK: {
            CHECK_INTERFACE(IProCameraUser, data, reply);
            reply->writeInt32(hasExclusiveLock());
            return NO_ERROR;
        } break;
        case SUBMIT_REQUEST: {
            CHECK_INTERFACE(IProCameraUser, data, reply);
            camera_metadata_t* metadata;
            readMetadata(data, /*out*/&metadata);

            // arg2 = streaming (bool)
            bool streaming = data.readInt32();

            // return code: requestId (int32)
            reply->writeInt32(submitRequest(metadata, streaming));

            return NO_ERROR;
        } break;
        case CANCEL_REQUEST: {
            CHECK_INTERFACE(IProCameraUser, data, reply);
            int requestId = data.readInt32();
            reply->writeInt32(cancelRequest(requestId));
            return NO_ERROR;
        } break;
        case REQUEST_STREAM: {
            CHECK_INTERFACE(IProCameraUser, data, reply);
            int streamId = data.readInt32();
            reply->writeInt32(requestStream(streamId));
            return NO_ERROR;
        } break;
        case CANCEL_STREAM: {
            CHECK_INTERFACE(IProCameraUser, data, reply);
            int streamId = data.readInt32();
            reply->writeInt32(cancelStream(streamId));
            return NO_ERROR;
        } break;
        case CREATE_STREAM: {
            CHECK_INTERFACE(IProCameraUser, data, reply);
            int width, height, format;

            width = data.readInt32();
            height = data.readInt32();
            format = data.readInt32();

            sp<Surface> surface = Surface::readFromParcel(data);

            int streamId = -1;
            status_t ret;
            ret = createStream(width, height, format, surface, &streamId);

            reply->writeInt32(streamId);
            reply->writeInt32(ret);

            return NO_ERROR;
        } break;

        case CREATE_DEFAULT_REQUEST: {
            CHECK_INTERFACE(IProCameraUser, data, reply);

            int templateId = data.readInt32();

            camera_metadata_t* request = NULL;
            status_t ret;
            ret = createDefaultRequest(templateId, &request);

            writeMetadata(*reply, request);
            reply->writeInt32(ret);

            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

}; // namespace android
