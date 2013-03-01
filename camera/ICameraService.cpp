/*
**
** Copyright 2008, The Android Open Source Project
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

#include <stdint.h>
#include <sys/types.h>

#include <binder/Parcel.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>

#include <camera/ICameraService.h>
#include <camera/ICameraServiceListener.h>
#include <camera/IProCameraUser.h>
#include <camera/IProCameraCallbacks.h>
#include <camera/ICamera.h>
#include <camera/ICameraClient.h>

namespace android {

class BpCameraService: public BpInterface<ICameraService>
{
public:
    BpCameraService(const sp<IBinder>& impl)
        : BpInterface<ICameraService>(impl)
    {
    }

    // get number of cameras available
    virtual int32_t getNumberOfCameras()
    {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        remote()->transact(BnCameraService::GET_NUMBER_OF_CAMERAS, data, &reply);
        return reply.readInt32();
    }

    // get information about a camera
    virtual status_t getCameraInfo(int cameraId,
                                   struct CameraInfo* cameraInfo) {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeInt32(cameraId);
        remote()->transact(BnCameraService::GET_CAMERA_INFO, data, &reply);
        cameraInfo->facing = reply.readInt32();
        cameraInfo->orientation = reply.readInt32();
        return reply.readInt32();
    }

    // connect to camera service
    virtual sp<ICamera> connect(const sp<ICameraClient>& cameraClient, int cameraId,
                                const String16 &clientPackageName, int clientUid)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeStrongBinder(cameraClient->asBinder());
        data.writeInt32(cameraId);
        data.writeString16(clientPackageName);
        data.writeInt32(clientUid);
        remote()->transact(BnCameraService::CONNECT, data, &reply);
        return interface_cast<ICamera>(reply.readStrongBinder());
    }

    // connect to camera service (pro client)
    virtual sp<IProCameraUser> connect(const sp<IProCameraCallbacks>& cameraCb, int cameraId,
                                       const String16 &clientPackageName, int clientUid)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeStrongBinder(cameraCb->asBinder());
        data.writeInt32(cameraId);
        data.writeString16(clientPackageName);
        data.writeInt32(clientUid);
        remote()->transact(BnCameraService::CONNECT_PRO, data, &reply);
        return interface_cast<IProCameraUser>(reply.readStrongBinder());
    }

    virtual status_t addListener(const sp<ICameraServiceListener>& listener)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeStrongBinder(listener->asBinder());
        remote()->transact(BnCameraService::ADD_LISTENER, data, &reply);
        return reply.readInt32();
    }

    virtual status_t removeListener(const sp<ICameraServiceListener>& listener)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeStrongBinder(listener->asBinder());
        remote()->transact(BnCameraService::REMOVE_LISTENER, data, &reply);
        return reply.readInt32();
    }
};

IMPLEMENT_META_INTERFACE(CameraService, "android.hardware.ICameraService");

// ----------------------------------------------------------------------

status_t BnCameraService::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch(code) {
        case GET_NUMBER_OF_CAMERAS: {
            CHECK_INTERFACE(ICameraService, data, reply);
            reply->writeInt32(getNumberOfCameras());
            return NO_ERROR;
        } break;
        case GET_CAMERA_INFO: {
            CHECK_INTERFACE(ICameraService, data, reply);
            CameraInfo cameraInfo;
            memset(&cameraInfo, 0, sizeof(cameraInfo));
            status_t result = getCameraInfo(data.readInt32(), &cameraInfo);
            reply->writeInt32(cameraInfo.facing);
            reply->writeInt32(cameraInfo.orientation);
            reply->writeInt32(result);
            return NO_ERROR;
        } break;
        case CONNECT: {
            CHECK_INTERFACE(ICameraService, data, reply);
            sp<ICameraClient> cameraClient =
                    interface_cast<ICameraClient>(data.readStrongBinder());
            int32_t cameraId = data.readInt32();
            const String16 clientName = data.readString16();
            int32_t clientUid = data.readInt32();
            sp<ICamera> camera = connect(cameraClient, cameraId,
                    clientName, clientUid);
            reply->writeStrongBinder(camera->asBinder());
            return NO_ERROR;
        } break;
        case CONNECT_PRO: {
            CHECK_INTERFACE(ICameraService, data, reply);
            sp<IProCameraCallbacks> cameraClient = interface_cast<IProCameraCallbacks>(data.readStrongBinder());
            int32_t cameraId = data.readInt32();
            const String16 clientName = data.readString16();
            int32_t clientUid = data.readInt32();
            sp<IProCameraUser> camera = connect(cameraClient, cameraId,
                                                clientName, clientUid);
            reply->writeStrongBinder(camera->asBinder());
            return NO_ERROR;
        } break;
        case ADD_LISTENER: {
            CHECK_INTERFACE(ICameraService, data, reply);
            sp<ICameraServiceListener> listener =
                interface_cast<ICameraServiceListener>(data.readStrongBinder());
            reply->writeInt32(addListener(listener));
            return NO_ERROR;
        } break;
        case REMOVE_LISTENER: {
            CHECK_INTERFACE(ICameraService, data, reply);
            sp<ICameraServiceListener> listener =
                interface_cast<ICameraServiceListener>(data.readStrongBinder());
            reply->writeInt32(removeListener(listener));
            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

}; // namespace android
