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

#ifndef ANDROID_SERVERS_CAMERA_CAMERAFLASHLIGHT_H
#define ANDROID_SERVERS_CAMERA_CAMERAFLASHLIGHT_H

#include "hardware/camera_common.h"
#include "utils/KeyedVector.h"
#include "gui/GLConsumer.h"
#include "gui/Surface.h"
#include "common/CameraDeviceBase.h"

namespace android {

/**
 * FlashControlBase is a base class for flash control. It defines the functions
 * that a flash control for each camera module/device version should implement.
 */
class FlashControlBase : public virtual VirtualLightRefBase {
    public:
        virtual ~FlashControlBase();

        // Whether a camera device has a flash unit. Calling this function may
        // cause the torch mode to be turned off in HAL v1 devices. If
        // previously-on torch mode is turned off,
        // callbacks.torch_mode_status_change() should be invoked.
        virtual status_t hasFlashUnit(const String16& cameraId,
                    bool *hasFlash) = 0;

        // set the torch mode to on or off.
        virtual status_t setTorchMode(const String16& cameraId,
                    bool enabled) = 0;
};

/**
 * CameraFlashlight can be used by camera service to control flashflight.
 */
class CameraFlashlight : public virtual VirtualLightRefBase {
    public:
        CameraFlashlight(CameraModule& cameraModule,
                const camera_module_callbacks_t& callbacks);
        virtual ~CameraFlashlight();

        // set the torch mode to on or off.
        status_t setTorchMode(const String16& cameraId, bool enabled);

        // Whether a camera device has a flash unit. Calling this function may
        // cause the torch mode to be turned off in HAL v1 devices.
        bool hasFlashUnit(const String16& cameraId);

        // Notify CameraFlashlight that camera service is going to open a camera
        // device. CameraFlashlight will free the resources that may cause the
        // camera open to fail. Camera service must call this function before
        // opening a camera device.
        status_t prepareDeviceOpen();

    private:
        // create flashlight control based on camera module API and camera
        // device API versions.
        status_t createFlashlightControl(const String16& cameraId);

        sp<FlashControlBase> mFlashControl;
        CameraModule *mCameraModule;
        const camera_module_callbacks_t *mCallbacks;

        Mutex mLock;
};

/**
 * Flash control for camera module v2.4 and above.
 */
class FlashControl : public FlashControlBase {
    public:
        FlashControl(CameraModule& cameraModule,
                const camera_module_callbacks_t& callbacks);
        virtual ~FlashControl();

        // FlashControlBase
        status_t hasFlashUnit(const String16& cameraId, bool *hasFlash);
        status_t setTorchMode(const String16& cameraId, bool enabled);

    private:
        CameraModule *mCameraModule;

        Mutex mLock;
};

/**
 * Flash control for camera module <= v2.3 and camera HAL v2-v3
 */
class CameraDeviceClientFlashControl : public FlashControlBase {
    public:
        CameraDeviceClientFlashControl(CameraModule& cameraModule,
                const camera_module_callbacks_t& callbacks);
        virtual ~CameraDeviceClientFlashControl();

        // FlashControlBase
        status_t setTorchMode(const String16& cameraId, bool enabled);
        status_t hasFlashUnit(const String16& cameraId, bool *hasFlash);

    private:
        // connect to a camera device
        status_t connectCameraDevice(const String16& cameraId);

        // initialize a surface
        status_t initializeSurface(int32_t width, int32_t height);

        // submit a request with the given torch mode
        status_t submitTorchRequest(bool enabled);

        // get the smallest surface size of IMPLEMENTATION_DEFINED
        status_t getSmallestSurfaceSize(const camera_info& info, int32_t *width,
                    int32_t *height);

        status_t hasFlashUnitLocked(const String16& cameraId, bool *hasFlash);

        CameraModule *mCameraModule;
        const camera_module_callbacks_t *mCallbacks;
        String16 mCameraId;
        bool mTorchEnabled;
        CameraMetadata *mMetadata;

        sp<CameraDeviceBase> mDevice;

        sp<IGraphicBufferProducer> mProducer;
        sp<IGraphicBufferConsumer>  mConsumer;
        sp<GLConsumer> mSurfaceTexture;
        sp<ANativeWindow> mAnw;
        int32_t mStreamId;

        Mutex mLock;
};

} // namespace android

#endif
