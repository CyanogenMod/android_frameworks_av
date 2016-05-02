/*
 * Copyright (C) 2011 The Android Open Source Project
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
#define LOG_TAG "ICameraRecordingProxyListener"
#include <camera/CameraUtils.h>
#include <camera/ICameraRecordingProxyListener.h>
#include <binder/IMemory.h>
#include <binder/Parcel.h>
#include <media/hardware/HardwareAPI.h>
#include <utils/Log.h>

namespace android {

enum {
    DATA_CALLBACK_TIMESTAMP = IBinder::FIRST_CALL_TRANSACTION,
    RECORDING_FRAME_HANDLE_CALLBACK_TIMESTAMP,
};

class BpCameraRecordingProxyListener: public BpInterface<ICameraRecordingProxyListener>
{
public:
    BpCameraRecordingProxyListener(const sp<IBinder>& impl)
        : BpInterface<ICameraRecordingProxyListener>(impl)
    {
    }

    void dataCallbackTimestamp(nsecs_t timestamp, int32_t msgType, const sp<IMemory>& imageData)
    {
        ALOGV("dataCallback");
        Parcel data, reply;
        data.writeInterfaceToken(ICameraRecordingProxyListener::getInterfaceDescriptor());
        data.writeInt64(timestamp);
        data.writeInt32(msgType);
        data.writeStrongBinder(IInterface::asBinder(imageData));
        remote()->transact(DATA_CALLBACK_TIMESTAMP, data, &reply, IBinder::FLAG_ONEWAY);
    }

    void recordingFrameHandleCallbackTimestamp(nsecs_t timestamp, native_handle_t* handle) {
        ALOGV("recordingFrameHandleCallbackTimestamp");
        Parcel data, reply;
        data.writeInterfaceToken(ICameraRecordingProxyListener::getInterfaceDescriptor());
        data.writeInt64(timestamp);
        data.writeNativeHandle(handle);
        remote()->transact(RECORDING_FRAME_HANDLE_CALLBACK_TIMESTAMP, data, &reply,
                IBinder::FLAG_ONEWAY);

        // The native handle is dupped in ICameraClient so we need to free it here.
        native_handle_close(handle);
        native_handle_delete(handle);
    }
};

IMPLEMENT_META_INTERFACE(CameraRecordingProxyListener, "android.hardware.ICameraRecordingProxyListener");

// ----------------------------------------------------------------------

status_t BnCameraRecordingProxyListener::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch(code) {
        case DATA_CALLBACK_TIMESTAMP: {
            ALOGV("DATA_CALLBACK_TIMESTAMP");
            CHECK_INTERFACE(ICameraRecordingProxyListener, data, reply);
            nsecs_t timestamp = data.readInt64();
            int32_t msgType = data.readInt32();
            sp<IMemory> imageData = interface_cast<IMemory>(data.readStrongBinder());
            dataCallbackTimestamp(timestamp, msgType, imageData);
            return NO_ERROR;
        } break;
        case RECORDING_FRAME_HANDLE_CALLBACK_TIMESTAMP: {
            ALOGV("RECORDING_FRAME_HANDLE_CALLBACK_TIMESTAMP");
            CHECK_INTERFACE(ICameraRecordingProxyListener, data, reply);
            nsecs_t timestamp;
            status_t res = data.readInt64(&timestamp);
            if (res != OK) {
                ALOGE("%s: Failed to read timestamp: %s (%d)", __FUNCTION__, strerror(-res), res);
                return BAD_VALUE;
            }

            native_handle_t* handle = data.readNativeHandle();
            if (handle == nullptr) {
                ALOGE("%s: Received a null native handle", __FUNCTION__);
                return BAD_VALUE;
            }
            // The native handle will be freed in
            // BpCameraRecordingProxy::releaseRecordingFrameHandle.
            recordingFrameHandleCallbackTimestamp(timestamp, handle);
            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

}; // namespace android

