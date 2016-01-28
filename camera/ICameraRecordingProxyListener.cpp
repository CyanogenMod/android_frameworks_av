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
        native_handle_t* nh = nullptr;

        if (CameraUtils::isNativeHandleMetadata(imageData)) {
            VideoNativeHandleMetadata *metadata =
                    (VideoNativeHandleMetadata*)(imageData->pointer());
            nh = metadata->pHandle;
            data.writeNativeHandle(nh);
        }

        remote()->transact(DATA_CALLBACK_TIMESTAMP, data, &reply, IBinder::FLAG_ONEWAY);

        // The native handle is dupped in ICameraClient so we need to free it here.
        if (nh) {
            native_handle_close(nh);
            native_handle_delete(nh);
        }
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

            if (CameraUtils::isNativeHandleMetadata(imageData)) {
                VideoNativeHandleMetadata *meta = (VideoNativeHandleMetadata*)(imageData->pointer());
                meta->pHandle = data.readNativeHandle();

                // The native handle will be freed in
                // BpCameraRecordingProxyListener::releaseRecordingFrame.
            }

            dataCallbackTimestamp(timestamp, msgType, imageData);
            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

}; // namespace android

