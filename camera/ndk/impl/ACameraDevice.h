/*
 * Copyright (C) 2015 The Android Open Source Project
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
#ifndef _ACAMERA_DEVICE_H
#define _ACAMERA_DEVICE_H

#include <memory>
#include <atomic>
#include <utils/StrongPointer.h>
#include <utils/Mutex.h>
#include <utils/String8.h>

#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/AMessage.h>
#include <camera/camera2/ICameraDeviceCallbacks.h>
#include <camera/camera2/ICameraDeviceUser.h>

#include <NdkCameraDevice.h>
#include "ACameraMetadata.h"


using namespace android;

namespace android {

struct CameraDevice final : public RefBase {
  public:
    CameraDevice(const char* id, ACameraDevice_StateCallbacks* cb,
                  std::unique_ptr<ACameraMetadata> chars,
                  ACameraDevice* wrapper);
    ~CameraDevice();

    inline const char* getId() const { return mCameraId.string(); }

    camera_status_t createCaptureRequest(
            ACameraDevice_request_template templateId,
            ACaptureRequest** request) const;

    // Callbacks from camera service
    class ServiceCallback : public BnCameraDeviceCallbacks {
      public:
        ServiceCallback(CameraDevice* device) : mDevice(device) {}
        void onDeviceError(CameraErrorCode errorCode,
                           const CaptureResultExtras& resultExtras) override;
        void onDeviceIdle() override;
        void onCaptureStarted(const CaptureResultExtras& resultExtras,
                              int64_t timestamp) override;
        void onResultReceived(const CameraMetadata& metadata,
                              const CaptureResultExtras& resultExtras) override;
        void onPrepared(int streamId) override;
      private:
        const wp<CameraDevice> mDevice;
    };
    inline sp<ICameraDeviceCallbacks> getServiceCallback() { return mServiceCallback; };

    // Camera device is only functional after remote being set
    void setRemoteDevice(sp<ICameraDeviceUser> remote);

  private:
    void disconnectLocked(); // disconnect from camera service
    camera_status_t checkCameraClosedOrErrorLocked() const;


    mutable Mutex mDeviceLock;
    const String8 mCameraId;                          // Camera ID
    const ACameraDevice_StateCallbacks mAppCallbacks; // Callback to app
    const std::unique_ptr<ACameraMetadata> mChars;    // Camera characteristics
    const sp<ServiceCallback> mServiceCallback;
    ACameraDevice* mWrapper;

    // TODO: maybe a bool will suffice for synchronous implementation?
    std::atomic_bool mClosing;
    inline bool isClosed() { return mClosing; }

    bool mInError;
    camera_status_t mError;
    void onCaptureErrorLocked(
            ICameraDeviceCallbacks::CameraErrorCode errorCode,
            const CaptureResultExtras& resultExtras);

    bool mIdle;

    sp<ICameraDeviceUser> mRemote;

    // Looper thread to handle callback to app
    sp<ALooper> mCbLooper;
    // definition of handler and message
    enum {
        kWhatOnDisconnected,
        kWhatOnError
    };
    static const char* kContextKey;
    static const char* kDeviceKey;
    static const char* kErrorCodeKey;
    static const char* kCallbackKey;
    class CallbackHandler : public AHandler {
      public:
        CallbackHandler() {}
        void onMessageReceived(const sp<AMessage> &msg) override;
    };
    sp<CallbackHandler> mHandler;

    inline ACameraDevice* getWrapper() { return mWrapper; };

    // TODO: might need another looper/handler to handle callbacks from service


};

} // namespace android;

/**
 * ACameraDevice opaque struct definition
 * Leave outside of android namespace because it's NDK struct
 */
struct ACameraDevice {
    ACameraDevice(const char* id, ACameraDevice_StateCallbacks* cb,
                  std::unique_ptr<ACameraMetadata> chars) :
            mDevice(new CameraDevice(id, cb, std::move(chars), this)) {}

    ~ACameraDevice() {};

    inline const char* getId() const { return mDevice->getId(); }

    camera_status_t createCaptureRequest(
            ACameraDevice_request_template templateId,
            ACaptureRequest** request) const {
        return mDevice->createCaptureRequest(templateId, request);
    }

    inline sp<ICameraDeviceCallbacks> getServiceCallback() {
        return mDevice->getServiceCallback();
    };

    // Camera device is only functional after remote being set
    inline void setRemoteDevice(sp<ICameraDeviceUser> remote) {
        mDevice->setRemoteDevice(remote);
    }

  private:
    // TODO: might need an API to give wp of mDevice to capture session
    sp<CameraDevice> mDevice;
};

#endif // _ACAMERA_DEVICE_H
