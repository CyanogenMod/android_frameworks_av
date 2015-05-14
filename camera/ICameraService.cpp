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

#define LOG_TAG "BpCameraService"
#include <utils/Log.h>
#include <utils/Errors.h>
#include <utils/String16.h>

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
#include <camera/camera2/ICameraDeviceUser.h>
#include <camera/camera2/ICameraDeviceCallbacks.h>
#include <camera/CameraMetadata.h>
#include <camera/VendorTagDescriptor.h>

namespace android {

namespace {

enum {
    EX_SECURITY = -1,
    EX_BAD_PARCELABLE = -2,
    EX_ILLEGAL_ARGUMENT = -3,
    EX_NULL_POINTER = -4,
    EX_ILLEGAL_STATE = -5,
    EX_HAS_REPLY_HEADER = -128,  // special; see below
};

static bool readExceptionCode(Parcel& reply) {
    int32_t exceptionCode = reply.readExceptionCode();

    if (exceptionCode != 0) {
        const char* errorMsg;
        switch(exceptionCode) {
            case EX_SECURITY:
                errorMsg = "Security";
                break;
            case EX_BAD_PARCELABLE:
                errorMsg = "BadParcelable";
                break;
            case EX_NULL_POINTER:
                errorMsg = "NullPointer";
                break;
            case EX_ILLEGAL_STATE:
                errorMsg = "IllegalState";
                break;
            // Binder should be handling this code inside Parcel::readException
            // but lets have a to-string here anyway just in case.
            case EX_HAS_REPLY_HEADER:
                errorMsg = "HasReplyHeader";
                break;
            default:
                errorMsg = "Unknown";
        }

        ALOGE("Binder transmission error %s (%d)", errorMsg, exceptionCode);
        return true;
    }

    return false;
}

};

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

        if (readExceptionCode(reply)) return 0;
        return reply.readInt32();
    }

    // get information about a camera
    virtual status_t getCameraInfo(int cameraId,
                                   struct CameraInfo* cameraInfo) {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeInt32(cameraId);
        remote()->transact(BnCameraService::GET_CAMERA_INFO, data, &reply);

        if (readExceptionCode(reply)) return -EPROTO;
        status_t result = reply.readInt32();
        if (reply.readInt32() != 0) {
            cameraInfo->facing = reply.readInt32();
            cameraInfo->orientation = reply.readInt32();
        }
        return result;
    }

    // get camera characteristics (static metadata)
    virtual status_t getCameraCharacteristics(int cameraId,
                                              CameraMetadata* cameraInfo) {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeInt32(cameraId);
        remote()->transact(BnCameraService::GET_CAMERA_CHARACTERISTICS, data, &reply);

        if (readExceptionCode(reply)) return -EPROTO;
        status_t result = reply.readInt32();

        CameraMetadata out;
        if (reply.readInt32() != 0) {
            out.readFromParcel(&reply);
        }

        if (cameraInfo != NULL) {
            cameraInfo->swap(out);
        }

        return result;
    }

    // Get enumeration and description of vendor tags for camera
    virtual status_t getCameraVendorTagDescriptor(/*out*/sp<VendorTagDescriptor>& desc) {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        remote()->transact(BnCameraService::GET_CAMERA_VENDOR_TAG_DESCRIPTOR, data, &reply);

        if (readExceptionCode(reply)) return -EPROTO;
        status_t result = reply.readInt32();

        if (reply.readInt32() != 0) {
            sp<VendorTagDescriptor> d;
            if (VendorTagDescriptor::createFromParcel(&reply, /*out*/d) == OK) {
                desc = d;
            }
        }
        return result;
    }

    // connect to camera service (android.hardware.Camera)
    virtual status_t connect(const sp<ICameraClient>& cameraClient, int cameraId,
                             const String16 &clientPackageName, int clientUid,
                             /*out*/
                             sp<ICamera>& device)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeStrongBinder(cameraClient->asBinder());
        data.writeInt32(cameraId);
        data.writeString16(clientPackageName);
        data.writeInt32(clientUid);
        remote()->transact(BnCameraService::CONNECT, data, &reply);

        if (readExceptionCode(reply)) return -EPROTO;
        status_t status = reply.readInt32();
        if (reply.readInt32() != 0) {
            device = interface_cast<ICamera>(reply.readStrongBinder());
        }
        return status;
    }

    // connect to camera service (android.hardware.Camera)
    virtual status_t connectLegacy(const sp<ICameraClient>& cameraClient, int cameraId,
                             int halVersion,
                             const String16 &clientPackageName, int clientUid,
                             /*out*/sp<ICamera>& device)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeStrongBinder(cameraClient->asBinder());
        data.writeInt32(cameraId);
        data.writeInt32(halVersion);
        data.writeString16(clientPackageName);
        data.writeInt32(clientUid);
        remote()->transact(BnCameraService::CONNECT_LEGACY, data, &reply);

        if (readExceptionCode(reply)) return -EPROTO;
        status_t status = reply.readInt32();
        if (reply.readInt32() != 0) {
            device = interface_cast<ICamera>(reply.readStrongBinder());
        }
        return status;
    }

    // connect to camera service (pro client)
    virtual status_t connectPro(const sp<IProCameraCallbacks>& cameraCb, int cameraId,
                                const String16 &clientPackageName, int clientUid,
                                /*out*/
                                sp<IProCameraUser>& device)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeStrongBinder(cameraCb->asBinder());
        data.writeInt32(cameraId);
        data.writeString16(clientPackageName);
        data.writeInt32(clientUid);
        remote()->transact(BnCameraService::CONNECT_PRO, data, &reply);

        if (readExceptionCode(reply)) return -EPROTO;
        status_t status = reply.readInt32();
        if (reply.readInt32() != 0) {
            device = interface_cast<IProCameraUser>(reply.readStrongBinder());
        }
        return status;
    }

    // connect to camera service (android.hardware.camera2.CameraDevice)
    virtual status_t connectDevice(
            const sp<ICameraDeviceCallbacks>& cameraCb,
            int cameraId,
            const String16& clientPackageName,
            int clientUid,
            /*out*/
            sp<ICameraDeviceUser>& device)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeStrongBinder(cameraCb->asBinder());
        data.writeInt32(cameraId);
        data.writeString16(clientPackageName);
        data.writeInt32(clientUid);
        remote()->transact(BnCameraService::CONNECT_DEVICE, data, &reply);

        if (readExceptionCode(reply)) return -EPROTO;
        status_t status = reply.readInt32();
        if (reply.readInt32() != 0) {
            device = interface_cast<ICameraDeviceUser>(reply.readStrongBinder());
        }
        return status;
    }

    virtual status_t addListener(const sp<ICameraServiceListener>& listener)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeStrongBinder(listener->asBinder());
        remote()->transact(BnCameraService::ADD_LISTENER, data, &reply);

        if (readExceptionCode(reply)) return -EPROTO;
        return reply.readInt32();
    }

    virtual status_t removeListener(const sp<ICameraServiceListener>& listener)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraService::getInterfaceDescriptor());
        data.writeStrongBinder(listener->asBinder());
        remote()->transact(BnCameraService::REMOVE_LISTENER, data, &reply);

        if (readExceptionCode(reply)) return -EPROTO;
        return reply.readInt32();
    }

    virtual status_t getLegacyParameters(int cameraId, String16* parameters) {
        if (parameters == NULL) {
            ALOGE("%s: parameters must not be null", __FUNCTION__);
            return BAD_VALUE;
        }

        Parcel data, reply;

        data.writeInt32(cameraId);
        remote()->transact(BnCameraService::GET_LEGACY_PARAMETERS, data, &reply);
        if (readExceptionCode(reply)) return -EPROTO;

        status_t res = data.readInt32();
        int32_t length = data.readInt32(); // -1 means null
        if (length > 0) {
            *parameters = data.readString16();
        } else {
            *parameters = String16();
        }

        return res;
    }

    virtual status_t supportsCameraApi(int cameraId, int apiVersion) {
        Parcel data, reply;

        data.writeInt32(cameraId);
        data.writeInt32(apiVersion);
        remote()->transact(BnCameraService::SUPPORTS_CAMERA_API, data, &reply);
        if (readExceptionCode(reply)) return -EPROTO;

        status_t res = data.readInt32();
        return res;
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
            reply->writeNoException();
            reply->writeInt32(getNumberOfCameras());
            return NO_ERROR;
        } break;
        case GET_CAMERA_INFO: {
            CHECK_INTERFACE(ICameraService, data, reply);
            CameraInfo cameraInfo = CameraInfo();
            memset(&cameraInfo, 0, sizeof(cameraInfo));
            status_t result = getCameraInfo(data.readInt32(), &cameraInfo);
            reply->writeNoException();
            reply->writeInt32(result);

            // Fake a parcelable object here
            reply->writeInt32(1); // means the parcelable is included
            reply->writeInt32(cameraInfo.facing);
            reply->writeInt32(cameraInfo.orientation);
            return NO_ERROR;
        } break;
        case GET_CAMERA_CHARACTERISTICS: {
            CHECK_INTERFACE(ICameraService, data, reply);
            CameraMetadata info;
            status_t result = getCameraCharacteristics(data.readInt32(), &info);
            reply->writeNoException();
            reply->writeInt32(result);

            // out-variables are after exception and return value
            reply->writeInt32(1); // means the parcelable is included
            info.writeToParcel(reply);
            return NO_ERROR;
        } break;
        case GET_CAMERA_VENDOR_TAG_DESCRIPTOR: {
            CHECK_INTERFACE(ICameraService, data, reply);
            sp<VendorTagDescriptor> d;
            status_t result = getCameraVendorTagDescriptor(d);
            reply->writeNoException();
            reply->writeInt32(result);

            // out-variables are after exception and return value
            if (d == NULL) {
                reply->writeInt32(0);
            } else {
                reply->writeInt32(1); // means the parcelable is included
                d->writeToParcel(reply);
            }
            return NO_ERROR;
        } break;
        case CONNECT: {
            CHECK_INTERFACE(ICameraService, data, reply);
            sp<ICameraClient> cameraClient =
                    interface_cast<ICameraClient>(data.readStrongBinder());
            int32_t cameraId = data.readInt32();
            const String16 clientName = data.readString16();
            int32_t clientUid = data.readInt32();
            sp<ICamera> camera;
            status_t status = connect(cameraClient, cameraId,
                    clientName, clientUid, /*out*/camera);
            reply->writeNoException();
            reply->writeInt32(status);
            if (camera != NULL) {
                reply->writeInt32(1);
                reply->writeStrongBinder(camera->asBinder());
            } else {
                reply->writeInt32(0);
            }
            return NO_ERROR;
        } break;
        case CONNECT_PRO: {
            CHECK_INTERFACE(ICameraService, data, reply);
            sp<IProCameraCallbacks> cameraClient =
                interface_cast<IProCameraCallbacks>(data.readStrongBinder());
            int32_t cameraId = data.readInt32();
            const String16 clientName = data.readString16();
            int32_t clientUid = data.readInt32();
            sp<IProCameraUser> camera;
            status_t status = connectPro(cameraClient, cameraId,
                    clientName, clientUid, /*out*/camera);
            reply->writeNoException();
            reply->writeInt32(status);
            if (camera != NULL) {
                reply->writeInt32(1);
                reply->writeStrongBinder(camera->asBinder());
            } else {
                reply->writeInt32(0);
            }
            return NO_ERROR;
        } break;
        case CONNECT_DEVICE: {
            CHECK_INTERFACE(ICameraService, data, reply);
            sp<ICameraDeviceCallbacks> cameraClient =
                interface_cast<ICameraDeviceCallbacks>(data.readStrongBinder());
            int32_t cameraId = data.readInt32();
            const String16 clientName = data.readString16();
            int32_t clientUid = data.readInt32();
            sp<ICameraDeviceUser> camera;
            status_t status = connectDevice(cameraClient, cameraId,
                    clientName, clientUid, /*out*/camera);
            reply->writeNoException();
            reply->writeInt32(status);
            if (camera != NULL) {
                reply->writeInt32(1);
                reply->writeStrongBinder(camera->asBinder());
            } else {
                reply->writeInt32(0);
            }
            return NO_ERROR;
        } break;
        case ADD_LISTENER: {
            CHECK_INTERFACE(ICameraService, data, reply);
            sp<ICameraServiceListener> listener =
                interface_cast<ICameraServiceListener>(data.readStrongBinder());
            reply->writeNoException();
            reply->writeInt32(addListener(listener));
            return NO_ERROR;
        } break;
        case REMOVE_LISTENER: {
            CHECK_INTERFACE(ICameraService, data, reply);
            sp<ICameraServiceListener> listener =
                interface_cast<ICameraServiceListener>(data.readStrongBinder());
            reply->writeNoException();
            reply->writeInt32(removeListener(listener));
            return NO_ERROR;
        } break;
        case GET_LEGACY_PARAMETERS: {
            CHECK_INTERFACE(ICameraService, data, reply);
            int cameraId = data.readInt32();
            String16 parameters;

            reply->writeNoException();
            // return value
            reply->writeInt32(getLegacyParameters(cameraId, &parameters));
            // out parameters
            reply->writeInt32(1); // parameters is always available
            reply->writeString16(parameters);
            return NO_ERROR;
        } break;
        case SUPPORTS_CAMERA_API: {
            CHECK_INTERFACE(ICameraService, data, reply);
            int cameraId = data.readInt32();
            int apiVersion = data.readInt32();

            reply->writeNoException();
            // return value
            reply->writeInt32(supportsCameraApi(cameraId, apiVersion));
            return NO_ERROR;
        } break;
        case CONNECT_LEGACY: {
            CHECK_INTERFACE(ICameraService, data, reply);
            sp<ICameraClient> cameraClient =
                    interface_cast<ICameraClient>(data.readStrongBinder());
            int32_t cameraId = data.readInt32();
            int32_t halVersion = data.readInt32();
            const String16 clientName = data.readString16();
            int32_t clientUid = data.readInt32();
            sp<ICamera> camera;
            status_t status = connectLegacy(cameraClient, cameraId, halVersion,
                    clientName, clientUid, /*out*/camera);
            reply->writeNoException();
            reply->writeInt32(status);
            if (camera != NULL) {
                reply->writeInt32(1);
                reply->writeStrongBinder(camera->asBinder());
            } else {
                reply->writeInt32(0);
            }
            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

}; // namespace android
