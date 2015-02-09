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

#define LOG_TAG "CameraFlashlight"
#define ATRACE_TAG ATRACE_TAG_CAMERA
#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>
#include <cutils/properties.h>

#include "camera/CameraMetadata.h"
#include "CameraFlashlight.h"
#include "gui/IGraphicBufferConsumer.h"
#include "gui/BufferQueue.h"
#include "camera/camera2/CaptureRequest.h"
#include "CameraDeviceFactory.h"


namespace android {

CameraFlashlight::CameraFlashlight(CameraModule& cameraModule,
        const camera_module_callbacks_t& callbacks) :
        mCameraModule(&cameraModule),
        mCallbacks(&callbacks) {
}

CameraFlashlight::~CameraFlashlight() {
}

status_t CameraFlashlight::createFlashlightControl(const String16& cameraId) {
    ALOGV("%s: creating a flash light control for camera %s", __FUNCTION__,
            cameraId.string());
    if (mFlashControl != NULL) {
        return INVALID_OPERATION;
    }

    status_t res = OK;

    if (mCameraModule->getRawModule()->module_api_version >=
            CAMERA_MODULE_API_VERSION_2_4) {
        mFlashControl = new FlashControl(*mCameraModule, *mCallbacks);
        if (mFlashControl == NULL) {
            ALOGV("%s: cannot create flash control for module api v2.4+",
                     __FUNCTION__);
            return NO_MEMORY;
        }
    } else {
        uint32_t deviceVersion = CAMERA_DEVICE_API_VERSION_1_0;

        if (mCameraModule->getRawModule()->module_api_version >=
                CAMERA_MODULE_API_VERSION_2_0) {
            camera_info info;
            res = mCameraModule->getCameraInfo(
                    atoi(String8(cameraId).string()), &info);
            if (res) {
                ALOGV("%s: failed to get camera info for camera %s",
                        __FUNCTION__, cameraId.string());
                return res;
            }
            deviceVersion = info.device_version;
        }

        if (deviceVersion >= CAMERA_DEVICE_API_VERSION_2_0) {
            CameraDeviceClientFlashControl *flashControl =
                    new CameraDeviceClientFlashControl(*mCameraModule,
                                                       *mCallbacks);
            if (!flashControl) {
                return NO_MEMORY;
            }

            mFlashControl = flashControl;
        }
        else {
            // todo: implement for device api 1
            return INVALID_OPERATION;
        }
    }

    return OK;
}

status_t CameraFlashlight::setTorchMode(const String16& cameraId, bool enabled) {
    if (!mCameraModule) {
        return NO_INIT;
    }

    ALOGV("%s: set torch mode of camera %s to %d", __FUNCTION__,
            cameraId.string(), enabled);

    status_t res = OK;
    Mutex::Autolock l(mLock);

    if (mFlashControl == NULL) {
        res = createFlashlightControl(cameraId);
        if (res) {
            return res;
        }
        res =  mFlashControl->setTorchMode(cameraId, enabled);
        return res;
    }

    // if flash control already exists, turning on torch mode may fail if it's
    // tied to another camera device for module v2.3 and below.
    res = mFlashControl->setTorchMode(cameraId, enabled);
    if (res == BAD_INDEX) {
        // flash control is tied to another camera device, need to close it and
        // try again.
        mFlashControl.clear();
        res = createFlashlightControl(cameraId);
        if (res) {
            return res;
        }
        res = mFlashControl->setTorchMode(cameraId, enabled);
    }

    return res;
}

bool CameraFlashlight::hasFlashUnit(const String16& cameraId) {
    status_t res;

    Mutex::Autolock l(mLock);

    if (mFlashControl == NULL) {
        res = createFlashlightControl(cameraId);
        if (res) {
            ALOGE("%s: failed to create flash control for %s ",
                    __FUNCTION__, cameraId.string());
            return false;
        }
    }

    bool flashUnit = false;

    // if flash control already exists, querying if a camera device has a flash
    // unit may fail if it's module v1
    res = mFlashControl->hasFlashUnit(cameraId, &flashUnit);
    if (res == BAD_INDEX) {
        // need to close the flash control before query.
        mFlashControl.clear();
        res = createFlashlightControl(cameraId);
        if (res) {
            ALOGE("%s: failed to create flash control for %s ", __FUNCTION__,
                    cameraId.string());
            return false;
        }
        res = mFlashControl->hasFlashUnit(cameraId, &flashUnit);
        if (res) {
            flashUnit = false;
        }
    }

    return flashUnit;
}

status_t CameraFlashlight::prepareDeviceOpen() {
    ALOGV("%s: prepare for device open", __FUNCTION__);

    Mutex::Autolock l(mLock);

    if (mCameraModule && mCameraModule->getRawModule()->module_api_version <
            CAMERA_MODULE_API_VERSION_2_4) {
        // framework is going to open a camera device, all flash light control
        // should be closed for backward compatible support.
        if (mFlashControl != NULL) {
            mFlashControl.clear();
        }
    }

    return OK;
}


FlashControlBase::~FlashControlBase() {
}


FlashControl::FlashControl(CameraModule& cameraModule,
        const camera_module_callbacks_t& callbacks) :
    mCameraModule(&cameraModule) {
}

FlashControl::~FlashControl() {
}

status_t FlashControl::hasFlashUnit(const String16& cameraId, bool *hasFlash) {
    if (!hasFlash) {
        return BAD_VALUE;
    }

    *hasFlash = false;

    Mutex::Autolock l(mLock);

    if (!mCameraModule) {
        return NO_INIT;
    }

    camera_info info;
    status_t res = mCameraModule->getCameraInfo(atoi(String8(cameraId).string()),
            &info);
    if (res != 0) {
        return res;
    }

    CameraMetadata metadata;
    metadata = info.static_camera_characteristics;
    camera_metadata_entry flashAvailable =
            metadata.find(ANDROID_FLASH_INFO_AVAILABLE);
    if (flashAvailable.count == 1 && flashAvailable.data.u8[0] == 1) {
        *hasFlash = true;
    }

    return OK;
}

status_t FlashControl::setTorchMode(const String16& cameraId, bool enabled) {
    ALOGV("%s: set camera %s torch mode to %d", __FUNCTION__,
            cameraId.string(), enabled);

    Mutex::Autolock l(mLock);
    if (!mCameraModule) {
        return NO_INIT;
    }

    return mCameraModule->setTorchMode(String8(cameraId).string(), enabled);
}

CameraDeviceClientFlashControl::CameraDeviceClientFlashControl(
        CameraModule& cameraModule,
        const camera_module_callbacks_t& callbacks) :
        mCameraModule(&cameraModule),
        mCallbacks(&callbacks),
        mTorchEnabled(false),
        mMetadata(NULL) {
}

CameraDeviceClientFlashControl::~CameraDeviceClientFlashControl() {
    if (mDevice != NULL) {
        mDevice->flush();
        mDevice->deleteStream(mStreamId);
        mDevice.clear();
    }
    if (mMetadata) {
        delete mMetadata;
    }

    mAnw.clear();
    mSurfaceTexture.clear();
    mProducer.clear();
    mConsumer.clear();

    if (mTorchEnabled) {
        if (mCallbacks) {
            ALOGV("%s: notify the framework that torch was turned off",
                    __FUNCTION__);
            mCallbacks->torch_mode_status_change(mCallbacks,
                    String8(mCameraId).string(), TORCH_MODE_STATUS_OFF);
        }
    }
}

status_t CameraDeviceClientFlashControl::initializeSurface(int32_t width,
        int32_t height) {
    status_t res;
    BufferQueue::createBufferQueue(&mProducer, &mConsumer);

    mSurfaceTexture = new GLConsumer(mConsumer, 0, GLConsumer::TEXTURE_EXTERNAL,
            true, true);
    if (mSurfaceTexture == NULL) {
        return NO_MEMORY;
    }

    int32_t format = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;
    res = mSurfaceTexture->setDefaultBufferSize(width, height);
    if (res) {
        return res;
    }
    res = mSurfaceTexture->setDefaultBufferFormat(format);
    if (res) {
        return res;
    }

    bool useAsync = false;
    int32_t consumerUsage;
    res = mProducer->query(NATIVE_WINDOW_CONSUMER_USAGE_BITS, &consumerUsage);
    if (res) {
        return res;
    }

    if (consumerUsage & GraphicBuffer::USAGE_HW_TEXTURE) {
        useAsync = true;
    }

    mAnw = new Surface(mProducer, useAsync);
    if (mAnw == NULL) {
        return NO_MEMORY;
    }
    res = mDevice->createStream(mAnw, width, height, format, &mStreamId);
    if (res) {
        return res;
    }

    res = mDevice->configureStreams();
    if (res) {
        return res;
    }

    return res;
}

status_t CameraDeviceClientFlashControl::getSmallestSurfaceSize(
        const camera_info& info, int32_t *width, int32_t *height) {
    if (!width || !height) {
        return BAD_VALUE;
    }

    int32_t w = INT32_MAX;
    int32_t h = 1;

    CameraMetadata metadata;
    metadata = info.static_camera_characteristics;
    camera_metadata_entry streamConfigs =
            metadata.find(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);
    for (size_t i = 0; i < streamConfigs.count; i += 4) {
        int32_t fmt = streamConfigs.data.i32[i];
        if (fmt == ANDROID_SCALER_AVAILABLE_FORMATS_IMPLEMENTATION_DEFINED) {
            int32_t ww = streamConfigs.data.i32[i + 1];
            int32_t hh = streamConfigs.data.i32[i + 2];

            if (w* h > ww * hh) {
                w = ww;
                h = hh;
            }
        }
    }

    if (w == INT32_MAX) {
        return NAME_NOT_FOUND;
    }

    *width = w;
    *height = h;

    return OK;
}

status_t CameraDeviceClientFlashControl::connectCameraDevice(
        const String16& cameraId) {
    String8 id = String8(cameraId);
    camera_info info;
    status_t res = mCameraModule->getCameraInfo(atoi(id.string()), &info);
    if (res != 0) {
        ALOGE("%s: failed to get camera info for camera %s", __FUNCTION__,
                mCameraId.string());
        return res;
    }

    mDevice = CameraDeviceFactory::createDevice(atoi(id.string()));
    if (mDevice == NULL) {
        return NO_MEMORY;
    }

    res = mDevice->initialize(mCameraModule);
    if (res) {
        goto fail;
    }

    int32_t width, height;
    res = getSmallestSurfaceSize(info, &width, &height);
    if (res) {
        return res;
    }
    res = initializeSurface(width, height);
    if (res) {
        goto fail;
    }

    mCameraId = cameraId;

    return OK;

fail:
    mDevice.clear();
    return res;
}


status_t CameraDeviceClientFlashControl::hasFlashUnit(const String16& cameraId,
        bool *hasFlash) {
    ALOGV("%s: checking if camera %s has a flash unit", __FUNCTION__,
            cameraId.string());

    Mutex::Autolock l(mLock);
    return hasFlashUnitLocked(cameraId, hasFlash);

}

status_t CameraDeviceClientFlashControl::hasFlashUnitLocked(
        const String16& cameraId, bool *hasFlash) {
    if (!mCameraModule) {
        ALOGE("%s: camera module is NULL", __FUNCTION__);
        return NO_INIT;
    }

    if (!hasFlash) {
        return BAD_VALUE;
    }

    camera_info info;
    status_t res = mCameraModule->getCameraInfo(
            atoi(String8(cameraId).string()), &info);
    if (res != 0) {
        ALOGE("%s: failed to get camera info for camera %s", __FUNCTION__,
                cameraId.string());
        return res;
    }

    CameraMetadata metadata;
    metadata = info.static_camera_characteristics;
    camera_metadata_entry flashAvailable =
            metadata.find(ANDROID_FLASH_INFO_AVAILABLE);
    if (flashAvailable.count == 1 && flashAvailable.data.u8[0] == 1) {
        *hasFlash = true;
    }

    return OK;
}

status_t CameraDeviceClientFlashControl::submitTorchRequest(bool enabled) {
    status_t res;

    if (mMetadata == NULL) {
        mMetadata = new CameraMetadata();
        if (mMetadata == NULL) {
            return NO_MEMORY;
        }
        res = mDevice->createDefaultRequest(
                CAMERA3_TEMPLATE_PREVIEW, mMetadata);
        if (res) {
            return res;
        }
    }

    uint8_t torchOn = enabled ? ANDROID_FLASH_MODE_TORCH :
                                ANDROID_FLASH_MODE_OFF;

    mMetadata->update(ANDROID_FLASH_MODE, &torchOn, 1);
    mMetadata->update(ANDROID_REQUEST_OUTPUT_STREAMS, &mStreamId, 1);

    int32_t requestId = 0;
    mMetadata->update(ANDROID_REQUEST_ID, &requestId, 1);

    List<const CameraMetadata> metadataRequestList;
    metadataRequestList.push_back(*mMetadata);

    int64_t lastFrameNumber = 0;
    res = mDevice->captureList(metadataRequestList, &lastFrameNumber);

    return res;
}


status_t CameraDeviceClientFlashControl::setTorchMode(
        const String16& cameraId, bool enabled) {
    bool hasFlash = false;

    Mutex::Autolock l(mLock);
    status_t res = hasFlashUnitLocked(cameraId, &hasFlash);

    // pre-check
    if (enabled) {
        // invalid camera?
        if (res) {
            return -EINVAL;
        }
        // no flash unit?
        if (!hasFlash) {
            return -ENOSYS;
        }
        // already opened for a different device?
        if (mDevice != NULL && cameraId != mCameraId) {
            return BAD_INDEX;
        }
    } else if (mDevice == NULL || cameraId != mCameraId) {
        // disabling the torch mode of an un-opened or different device.
        return OK;
    }

    if (mDevice == NULL) {
        res = connectCameraDevice(cameraId);
        if (res) {
            return res;
        }
    }

    res = submitTorchRequest(enabled);
    if (res) {
        return res;
    }

    mTorchEnabled = enabled;
    return OK;
}

}
