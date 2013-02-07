/*
 * Copyright (C) 2013 The Android Open Source Project
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

#define LOG_TAG "Camera3-Device"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0
//#define LOG_NNDEBUG 0  // Per-frame verbose logging

#ifdef LOG_NNDEBUG
#define ALOGVV(...) ALOGV(__VA_ARGS__)
#else
#define ALOGVV(...) ((void)0)
#endif

#include <utils/Log.h>
#include <utils/Trace.h>
#include <utils/Timers.h>
#include "Camera3Device.h"

namespace android {


Camera3Device::Camera3Device(int id):
        mId(id),
        mHal3Device(NULL)
{
    ATRACE_CALL();
    camera3_callback_ops::notify = &sNotify;
    camera3_callback_ops::process_capture_result = &sProcessCaptureResult;
    ALOGV("%s: Created device for camera %d", __FUNCTION__, id);
}

Camera3Device::~Camera3Device()
{
    ATRACE_CALL();
    ALOGV("%s: Tearing down for camera id %d", __FUNCTION__, mId);
    disconnect();
}

status_t Camera3Device::initialize(camera_module_t *module)
{
    ATRACE_CALL();
    ALOGV("%s: Initializing device for camera %d", __FUNCTION__, mId);
    if (mHal3Device != NULL) {
        ALOGE("%s: Already initialized!", __FUNCTION__);
        return INVALID_OPERATION;
    }

    /** Open HAL device */

    status_t res;
    String8 deviceName = String8::format("%d", mId);

    camera3_device_t *device;

    res = module->common.methods->open(&module->common, deviceName.string(),
            reinterpret_cast<hw_device_t**>(&device));

    if (res != OK) {
        ALOGE("%s: Could not open camera %d: %s (%d)", __FUNCTION__,
                mId, strerror(-res), res);
        return res;
    }

    /** Cross-check device version */

    if (device->common.version != CAMERA_DEVICE_API_VERSION_3_0) {
        ALOGE("%s: Could not open camera %d: "
                "Camera device is not version %x, reports %x instead",
                __FUNCTION__, mId, CAMERA_DEVICE_API_VERSION_3_0,
                device->common.version);
        device->common.close(&device->common);
        return BAD_VALUE;
    }

    camera_info info;
    res = module->get_camera_info(mId, &info);
    if (res != OK) return res;

    if (info.device_version != device->common.version) {
        ALOGE("%s: HAL reporting mismatched camera_info version (%x)"
                " and device version (%x).", __FUNCTION__,
                device->common.version, info.device_version);
        device->common.close(&device->common);
        return BAD_VALUE;
    }

    /** Initialize device with callback functions */

    res = device->ops->initialize(device, this);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to initialize HAL device: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        device->common.close(&device->common);
        return BAD_VALUE;
    }

    /** Get vendor metadata tags */

    mVendorTagOps.get_camera_vendor_section_name = NULL;

    device->ops->get_metadata_vendor_tag_ops(device, &mVendorTagOps);

    if (mVendorTagOps.get_camera_vendor_section_name != NULL) {
        res = set_camera_metadata_vendor_tag_ops(&mVendorTagOps);
        if (res != OK) {
            ALOGE("%s: Camera %d: Unable to set tag ops: %s (%d)",
                    __FUNCTION__, mId, strerror(-res), res);
            device->common.close(&device->common);
            return res;
        }
    }

    /** Start up request queue thread */

    requestThread = new RequestThread(this);
    res = requestThread->run(String8::format("C3Dev-%d-ReqQueue", mId).string());
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to start request queue thread: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        device->common.close(&device->common);
        return res;
    }

    /** Everything is good to go */

    mDeviceInfo = info.static_camera_characteristics;
    mHal3Device = device;

    return OK;
}

status_t Camera3Device::disconnect() {
    ATRACE_CALL();

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::dump(int fd, const Vector<String16> &args) {
    ATRACE_CALL();
    (void)args;

    mHal3Device->ops->dump(mHal3Device, fd);

    return OK;
}

const CameraMetadata& Camera3Device::info() const {
    ALOGVV("%s: E", __FUNCTION__);

    return mDeviceInfo;
}

status_t Camera3Device::capture(CameraMetadata &request) {
    ATRACE_CALL();
    (void)request;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}


status_t Camera3Device::setStreamingRequest(const CameraMetadata &request) {
    ATRACE_CALL();
    (void)request;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::clearStreamingRequest() {
    ATRACE_CALL();

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::waitUntilRequestReceived(int32_t requestId, nsecs_t timeout) {
    ATRACE_CALL();
    (void)requestId; (void)timeout;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::createStream(sp<ANativeWindow> consumer,
        uint32_t width, uint32_t height, int format, size_t size, int *id) {
    ATRACE_CALL();
    (void)consumer; (void)width; (void)height; (void)format;
    (void)size; (void)id;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::createReprocessStreamFromStream(int outputId, int *id) {
    ATRACE_CALL();
    (void)outputId; (void)id;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}


status_t Camera3Device::getStreamInfo(int id,
        uint32_t *width, uint32_t *height, uint32_t *format) {
    ATRACE_CALL();
    (void)id; (void)width; (void)height; (void)format;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::setStreamTransform(int id,
        int transform) {
    ATRACE_CALL();
    (void)id; (void)transform;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::deleteStream(int id) {
    ATRACE_CALL();
    (void)id;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::deleteReprocessStream(int id) {
    ATRACE_CALL();
    (void)id;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}


status_t Camera3Device::createDefaultRequest(int templateId,
        CameraMetadata *request) {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);

    const camera_metadata_t *rawRequest;
    rawRequest = mHal3Device->ops->construct_default_request_settings(
        mHal3Device, templateId);
    if (rawRequest == NULL) return DEAD_OBJECT;
    *request = rawRequest;

    return OK;
}

status_t Camera3Device::waitUntilDrained() {
    ATRACE_CALL();

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::setNotifyCallback(NotificationListener *listener) {
    ATRACE_CALL();
    (void)listener;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::waitForNextFrame(nsecs_t timeout) {
    (void)timeout;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::getNextFrame(CameraMetadata *frame) {
    ATRACE_CALL();
    (void)frame;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::triggerAutofocus(uint32_t id) {
    ATRACE_CALL();
    (void)id;


    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t Camera3Device::triggerCancelAutofocus(uint32_t id) {
    ATRACE_CALL();
    (void)id;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;

}

status_t Camera3Device::triggerPrecaptureMetering(uint32_t id) {
    ATRACE_CALL();
    (void)id;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;

}

status_t Camera3Device::pushReprocessBuffer(int reprocessStreamId,
        buffer_handle_t *buffer, wp<BufferReleasedListener> listener) {
    ATRACE_CALL();
    (void)reprocessStreamId; (void)buffer; (void)listener;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

Camera3Device::RequestThread::RequestThread(wp<Camera3Device> parent) :
        Thread(false),
        mParent(parent) {
}

bool Camera3Device::RequestThread::threadLoop() {
    ALOGE("%s: Unimplemented", __FUNCTION__);

    return false;
}

void Camera3Device::processCaptureResult(const camera3_capture_result *result) {
    (void)result;

    ALOGE("%s: Unimplemented", __FUNCTION__);
}

void Camera3Device::notify(const camera3_notify_msg *msg) {
    (void)msg;

    ALOGE("%s: Unimplemented", __FUNCTION__);
}

/**
 * Static callback forwarding methods from HAL to instance
 */

void Camera3Device::sProcessCaptureResult(const camera3_callback_ops *cb,
        const camera3_capture_result *result) {
    Camera3Device *d =
            const_cast<Camera3Device*>(static_cast<const Camera3Device*>(cb));
    d->processCaptureResult(result);
}

void Camera3Device::sNotify(const camera3_callback_ops *cb,
        const camera3_notify_msg *msg) {
    Camera3Device *d =
            const_cast<Camera3Device*>(static_cast<const Camera3Device*>(cb));
    d->notify(msg);
}

}; // namespace android
