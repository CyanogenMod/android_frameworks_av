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
    DELETE_STREAM,
    CREATE_STREAM,
    CREATE_DEFAULT_REQUEST,
    GET_CAMERA_INFO,
};

/**
  * Caller becomes the owner of the new metadata
  * 'const Parcel' doesnt prevent us from calling the read functions.
  *  which is interesting since it changes the internal state
  *
  * NULL can be returned when no metadata was sent, OR if there was an issue
  * unpacking the serialized data (i.e. bad parcel or invalid structure).
  */
void readMetadata(const Parcel& data, camera_metadata_t** out) {

    status_t err = OK;

    camera_metadata_t* metadata = NULL;

    if (out) {
        *out = NULL;
    }

    // arg0 = metadataSize (int32)
    int32_t metadataSizeTmp = -1;
    if ((err = data.readInt32(&metadataSizeTmp)) != OK) {
        ALOGE("%s: Failed to read metadata size (error %d %s)",
              __FUNCTION__, err, strerror(-err));
        return;
    }
    const size_t metadataSize = static_cast<size_t>(metadataSizeTmp);

    if (metadataSize == 0) {
        return;
    }

    // NOTE: this doesn't make sense to me. shouldnt the blob
    // know how big it is? why do we have to specify the size
    // to Parcel::readBlob ?

    ReadableBlob blob;
    // arg1 = metadata (blob)
    do {
        if ((err = data.readBlob(metadataSize, &blob)) != OK) {
            ALOGE("%s: Failed to read metadata blob (sized %d). Possible "
                  " serialization bug. Error %d %s",
                  __FUNCTION__, metadataSize, err, strerror(-err));
            break;
        }
        const camera_metadata_t* tmp =
                       reinterpret_cast<const camera_metadata_t*>(blob.data());

        metadata = allocate_copy_camera_metadata_checked(tmp, metadataSize);
    } while(0);
    blob.release();

    if (out) {
        *out = metadata;
    } else if (metadata != NULL) {
        free_camera_metadata(metadata);
    }
}

/**
  * Caller retains ownership of metadata
  * - Write 2 (int32 + blob) args in the current position
  */
void writeMetadata(Parcel& data, camera_metadata_t* metadata) {
    // arg0 = metadataSize (int32)

    if (metadata == NULL) {
        data.writeInt32(0);
        return;
    }

    const size_t metadataSize = get_camera_metadata_compact_size(metadata);
    data.writeInt32(static_cast<int32_t>(metadataSize));

    // arg1 = metadata (blob)
    WritableBlob blob;
    {
        data.writeBlob(metadataSize, &blob);
        copy_camera_metadata(blob.data(), metadataSize, metadata);

        IF_ALOGV() {
            if (validate_camera_metadata_structure(
                        (const camera_metadata_t*)blob.data(),
                        &metadataSize) != OK) {
                ALOGV("%s: Failed to validate metadata %p after writing blob",
                       __FUNCTION__, blob.data());
            } else {
                ALOGV("%s: Metadata written to blob. Validation success",
                        __FUNCTION__);
            }
        }

        // Not too big of a problem since receiving side does hard validation
        if (validate_camera_metadata_structure(metadata, &metadataSize) != OK) {
            ALOGW("%s: Failed to validate metadata %p before writing blob",
                   __FUNCTION__, metadata);
        }

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

    virtual status_t deleteStream(int streamId)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        data.writeInt32(streamId);

        remote()->transact(DELETE_STREAM, data, &reply);
        return reply.readInt32();
    }

    virtual status_t createStream(int width, int height, int format,
                          const sp<IGraphicBufferProducer>& bufferProducer,
                          /*out*/
                          int* streamId)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        data.writeInt32(width);
        data.writeInt32(height);
        data.writeInt32(format);

        sp<IBinder> b(bufferProducer->asBinder());
        data.writeStrongBinder(b);

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


    virtual status_t getCameraInfo(int cameraId, camera_metadata** info)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IProCameraUser::getInterfaceDescriptor());
        data.writeInt32(cameraId);
        remote()->transact(GET_CAMERA_INFO, data, &reply);
        readMetadata(reply, /*out*/info);
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
        case DELETE_STREAM: {
            CHECK_INTERFACE(IProCameraUser, data, reply);
            int streamId = data.readInt32();
            reply->writeInt32(deleteStream(streamId));
            return NO_ERROR;
        } break;
        case CREATE_STREAM: {
            CHECK_INTERFACE(IProCameraUser, data, reply);
            int width, height, format;

            width = data.readInt32();
            height = data.readInt32();
            format = data.readInt32();

            sp<IGraphicBufferProducer> bp =
               interface_cast<IGraphicBufferProducer>(data.readStrongBinder());

            int streamId = -1;
            status_t ret;
            ret = createStream(width, height, format, bp, &streamId);

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

            free_camera_metadata(request);

            return NO_ERROR;
        } break;
        case GET_CAMERA_INFO: {
            CHECK_INTERFACE(IProCameraUser, data, reply);

            int cameraId = data.readInt32();

            camera_metadata_t* info = NULL;
            status_t ret;
            ret = getCameraInfo(cameraId, &info);

            writeMetadata(*reply, info);
            reply->writeInt32(ret);

            free_camera_metadata(info);

            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

}; // namespace android
