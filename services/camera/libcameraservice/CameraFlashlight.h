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
#include "utils/SortedVector.h"
#include "gui/GLConsumer.h"
#include "gui/Surface.h"
#include "common/CameraDeviceBase.h"
#include "device1/CameraHardwareInterface.h"

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
        virtual status_t hasFlashUnit(const String8& cameraId,
                    bool *hasFlash) = 0;

        // set the torch mode to on or off.
        virtual status_t setTorchMode(const String8& cameraId,
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

        // Find all flash units. This must be called before other methods. All
        // camera devices must be closed when it's called because HAL v1 devices
        // need to be opened to query available flash modes.
        status_t findFlashUnits();

        // Whether a camera device has a flash unit. Before findFlashUnits() is
        // called, this function always returns false.
        bool hasFlashUnit(const String8& cameraId);

        // set the torch mode to on or off.
        status_t setTorchMode(const String8& cameraId, bool enabled);

        // Notify CameraFlashlight that camera service is going to open a camera
        // device. CameraFlashlight will free the resources that may cause the
        // camera open to fail. Camera service must call this function before
        // opening a camera device.
        status_t prepareDeviceOpen(const String8& cameraId);

        // Notify CameraFlashlight that camera service has closed a camera
        // device. CameraFlashlight may invoke callbacks for torch mode
        // available depending on the implementation.
        status_t deviceClosed(const String8& cameraId);

    private:
        // create flashlight control based on camera module API and camera
        // device API versions.
        status_t createFlashlightControl(const String8& cameraId);

        // mLock should be locked.
        bool hasFlashUnitLocked(const String8& cameraId);

        sp<FlashControlBase> mFlashControl;
        CameraModule *mCameraModule;
        const camera_module_callbacks_t *mCallbacks;
        SortedVector<String8> mOpenedCameraIds;

        // camera id -> if it has a flash unit
        KeyedVector<String8, bool> mHasFlashlightMap;
        bool mFlashlightMapInitialized;

        Mutex mLock; // protect CameraFlashlight API
};

/**
 * Flash control for camera module v2.4 and above.
 */
class ModuleFlashControl : public FlashControlBase {
    public:
        ModuleFlashControl(CameraModule& cameraModule,
                const camera_module_callbacks_t& callbacks);
        virtual ~ModuleFlashControl();

        // FlashControlBase
        status_t hasFlashUnit(const String8& cameraId, bool *hasFlash);
        status_t setTorchMode(const String8& cameraId, bool enabled);

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
        status_t setTorchMode(const String8& cameraId, bool enabled);
        status_t hasFlashUnit(const String8& cameraId, bool *hasFlash);

    private:
        // connect to a camera device
        status_t connectCameraDevice(const String8& cameraId);
        // disconnect and free mDevice
        status_t disconnectCameraDevice();

        // initialize a surface
        status_t initializeSurface(sp<CameraDeviceBase>& device, int32_t width,
                int32_t height);

        // submit a request to enable the torch mode
        status_t submitTorchEnabledRequest();

        // get the smallest surface size of IMPLEMENTATION_DEFINED
        status_t getSmallestSurfaceSize(const camera_info& info, int32_t *width,
                    int32_t *height);

        // protected by mLock
        status_t hasFlashUnitLocked(const String8& cameraId, bool *hasFlash);

        CameraModule *mCameraModule;
        const camera_module_callbacks_t *mCallbacks;
        String8 mCameraId;
        bool mTorchEnabled;
        CameraMetadata *mMetadata;
        // WORKAROUND: will be set to true for HAL v2 devices where
        // setStreamingRequest() needs to be call for torch mode settings to
        // take effect.
        bool mStreaming;

        sp<CameraDeviceBase> mDevice;

        sp<IGraphicBufferProducer> mProducer;
        sp<IGraphicBufferConsumer>  mConsumer;
        sp<GLConsumer> mSurfaceTexture;
        sp<Surface> mSurface;
        int32_t mStreamId;

        Mutex mLock;
};

/**
 * Flash control for camera module <= v2.3 and camera HAL v1
 */
class CameraHardwareInterfaceFlashControl : public FlashControlBase {
    public:
        CameraHardwareInterfaceFlashControl(CameraModule& cameraModule,
                const camera_module_callbacks_t& callbacks);
        virtual ~CameraHardwareInterfaceFlashControl();

        // FlashControlBase
        status_t setTorchMode(const String8& cameraId, bool enabled);
        status_t hasFlashUnit(const String8& cameraId, bool *hasFlash);

    private:
        // connect to a camera device
        status_t connectCameraDevice(const String8& cameraId);

        // disconnect and free mDevice
        status_t disconnectCameraDevice();

        // initialize the preview window
        status_t initializePreviewWindow(sp<CameraHardwareInterface> device,
                int32_t width, int32_t height);

        // start preview and enable torch
        status_t startPreviewAndTorch();

        // get the smallest surface
        status_t getSmallestSurfaceSize(int32_t *width, int32_t *height);

        // protected by mLock
        status_t hasFlashUnitLocked(const String8& cameraId, bool *hasFlash);

        CameraModule *mCameraModule;
        const camera_module_callbacks_t *mCallbacks;
        sp<CameraHardwareInterface> mDevice;
        String8 mCameraId;
        CameraParameters mParameters;
        bool mTorchEnabled;

        sp<IGraphicBufferProducer> mProducer;
        sp<IGraphicBufferConsumer>  mConsumer;
        sp<GLConsumer> mSurfaceTexture;
        sp<Surface> mSurface;

        Mutex mLock;
};

} // namespace android

#endif
