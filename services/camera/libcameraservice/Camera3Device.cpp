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
#include "camera3/Camera3OutputStream.h"
#include "camera3/Camera3InputStream.h"

using namespace android::camera3;

namespace android {

Camera3Device::Camera3Device(int id):
        mId(id),
        mHal3Device(NULL),
        mStatus(STATUS_UNINITIALIZED),
        mListener(NULL)
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

int Camera3Device::getId() const {
    return mId;
}

/**
 * CameraDeviceBase interface
 */

status_t Camera3Device::initialize(camera_module_t *module)
{
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    ALOGV("%s: Initializing device for camera %d", __FUNCTION__, mId);
    if (mStatus != STATUS_UNINITIALIZED) {
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
        mStatus = STATUS_ERROR;
        return res;
    }

    /** Cross-check device version */

    if (device->common.version != CAMERA_DEVICE_API_VERSION_3_0) {
        ALOGE("%s: Could not open camera %d: "
                "Camera device is not version %x, reports %x instead",
                __FUNCTION__, mId, CAMERA_DEVICE_API_VERSION_3_0,
                device->common.version);
        device->common.close(&device->common);
        mStatus = STATUS_ERROR;
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
        mStatus = STATUS_ERROR;
        return BAD_VALUE;
    }

    /** Initialize device with callback functions */

    res = device->ops->initialize(device, this);
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to initialize HAL device: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        device->common.close(&device->common);
        mStatus = STATUS_ERROR;
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
            mStatus = STATUS_ERROR;
            return res;
        }
    }

    /** Start up request queue thread */

    mRequestThread = new RequestThread(this, device);
    res = mRequestThread->run(String8::format("C3Dev-%d-ReqQueue", mId).string());
    if (res != OK) {
        ALOGE("%s: Camera %d: Unable to start request queue thread: %s (%d)",
                __FUNCTION__, mId, strerror(-res), res);
        device->common.close(&device->common);
        mRequestThread.clear();
        mStatus = STATUS_ERROR;
        return res;
    }

    /** Everything is good to go */

    mDeviceInfo = info.static_camera_characteristics;
    mHal3Device = device;
    mStatus = STATUS_IDLE;
    mNextStreamId = 0;

    return OK;
}

status_t Camera3Device::disconnect() {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    ALOGV("%s: E", __FUNCTION__);

    status_t res;
    if (mStatus == STATUS_UNINITIALIZED) return OK;

    if (mStatus == STATUS_ACTIVE ||
            (mStatus == STATUS_ERROR && mRequestThread != NULL)) {
        res = mRequestThread->clearRepeatingRequests();
        if (res != OK) {
            ALOGE("%s: Can't stop streaming", __FUNCTION__);
            return res;
        }
        res = waitUntilDrainedLocked();
        if (res != OK) {
            ALOGE("%s: Timeout waiting for HAL to drain", __FUNCTION__);
            return res;
        }
    }
    assert(mStatus == STATUS_IDLE || mStatus == STATUS_ERROR);

    if (mRequestThread != NULL) {
        mRequestThread->requestExit();
    }

    mOutputStreams.clear();
    mInputStream.clear();

    if (mRequestThread != NULL) {
        mRequestThread->join();
        mRequestThread.clear();
    }

    if (mHal3Device != NULL) {
        mHal3Device->common.close(&mHal3Device->common);
        mHal3Device = NULL;
    }

    mStatus = STATUS_UNINITIALIZED;

    ALOGV("%s: X", __FUNCTION__);
    return OK;
}

status_t Camera3Device::dump(int fd, const Vector<String16> &args) {
    ATRACE_CALL();
    (void)args;
    String8 lines;

    const char *status =
            mStatus == STATUS_ERROR         ? "ERROR" :
            mStatus == STATUS_UNINITIALIZED ? "UNINITIALIZED" :
            mStatus == STATUS_IDLE          ? "IDLE" :
            mStatus == STATUS_ACTIVE        ? "ACTIVE" :
            "Unknown";
    lines.appendFormat("    Device status: %s\n", status);
    lines.appendFormat("    Stream configuration:\n");

    if (mInputStream != NULL) {
        write(fd, lines.string(), lines.size());
        mInputStream->dump(fd, args);
    } else {
        lines.appendFormat("      No input stream.\n");
        write(fd, lines.string(), lines.size());
    }
    for (size_t i = 0; i < mOutputStreams.size(); i++) {
        mOutputStreams[i]->dump(fd,args);
    }

    if (mHal3Device != NULL) {
        lines = String8("     HAL device dump:\n");
        write(fd, lines.string(), lines.size());
        mHal3Device->ops->dump(mHal3Device, fd);
    }

    return OK;
}

const CameraMetadata& Camera3Device::info() const {
    ALOGVV("%s: E", __FUNCTION__);
    if (CC_UNLIKELY(mStatus == STATUS_UNINITIALIZED ||
                    mStatus == STATUS_ERROR)) {
        ALOGE("%s: Access to static info %s!", __FUNCTION__,
                mStatus == STATUS_ERROR ?
                "when in error state" : "before init");
    }
    return mDeviceInfo;
}

status_t Camera3Device::capture(CameraMetadata &request) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    // TODO: take ownership of the request

    switch (mStatus) {
        case STATUS_ERROR:
            ALOGE("%s: Device has encountered a serious error", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            ALOGE("%s: Device not initialized", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_IDLE:
        case STATUS_ACTIVE:
            // OK
            break;
        default:
            ALOGE("%s: Unexpected status: %d", __FUNCTION__, mStatus);
            return INVALID_OPERATION;
    }

    sp<CaptureRequest> newRequest = setUpRequestLocked(request);
    if (newRequest == NULL) {
        ALOGE("%s: Can't create capture request", __FUNCTION__);
        return BAD_VALUE;
    }

    return mRequestThread->queueRequest(newRequest);
}


status_t Camera3Device::setStreamingRequest(const CameraMetadata &request) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    switch (mStatus) {
        case STATUS_ERROR:
            ALOGE("%s: Device has encountered a serious error", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            ALOGE("%s: Device not initialized", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_IDLE:
        case STATUS_ACTIVE:
            // OK
            break;
        default:
            ALOGE("%s: Unexpected status: %d", __FUNCTION__, mStatus);
            return INVALID_OPERATION;
    }

    sp<CaptureRequest> newRepeatingRequest = setUpRequestLocked(request);
    if (newRepeatingRequest == NULL) {
        ALOGE("%s: Can't create repeating request", __FUNCTION__);
        return BAD_VALUE;
    }

    RequestList newRepeatingRequests;
    newRepeatingRequests.push_back(newRepeatingRequest);

    return mRequestThread->setRepeatingRequests(newRepeatingRequests);
}


sp<Camera3Device::CaptureRequest> Camera3Device::setUpRequestLocked(
        const CameraMetadata &request) {
    status_t res;

    if (mStatus == STATUS_IDLE) {
        res = configureStreamsLocked();
        if (res != OK) {
            ALOGE("%s: Can't set up streams: %s (%d)",
                    __FUNCTION__, strerror(-res), res);
            return NULL;
        }
    }

    sp<CaptureRequest> newRequest = createCaptureRequest(request);
    return newRequest;
}

status_t Camera3Device::clearStreamingRequest() {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    switch (mStatus) {
        case STATUS_ERROR:
            ALOGE("%s: Device has encountered a serious error", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            ALOGE("%s: Device not initialized", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_IDLE:
        case STATUS_ACTIVE:
            // OK
            break;
        default:
            ALOGE("%s: Unexpected status: %d", __FUNCTION__, mStatus);
            return INVALID_OPERATION;
    }

    return mRequestThread->clearRepeatingRequests();
}

status_t Camera3Device::waitUntilRequestReceived(int32_t requestId, nsecs_t timeout) {
    ATRACE_CALL();

    return mRequestThread->waitUntilRequestProcessed(requestId, timeout);
}

status_t Camera3Device::createInputStream(
        uint32_t width, uint32_t height, int format, int *id) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    status_t res;
    bool wasActive = false;

    switch (mStatus) {
        case STATUS_ERROR:
            ALOGE("%s: Device has encountered a serious error", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            ALOGE("%s: Device not initialized", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_IDLE:
            // OK
            break;
        case STATUS_ACTIVE:
            ALOGV("%s: Stopping activity to reconfigure streams", __FUNCTION__);
            mRequestThread->setPaused(true);
            res = waitUntilDrainedLocked();
            if (res != OK) {
                ALOGE("%s: Can't pause captures to reconfigure streams!",
                        __FUNCTION__);
                mStatus = STATUS_ERROR;
                return res;
            }
            wasActive = true;
            break;
        default:
            ALOGE("%s: Unexpected status: %d", __FUNCTION__, mStatus);
            return INVALID_OPERATION;
    }
    assert(mStatus == STATUS_IDLE);

    if (mInputStream != 0) {
        ALOGE("%s: Cannot create more than 1 input stream", __FUNCTION__);
        return INVALID_OPERATION;
    }

    sp<Camera3InputStream> newStream = new Camera3InputStream(mNextStreamId,
                width, height, format);

    mInputStream = newStream;

    *id = mNextStreamId++;

    // Continue captures if active at start
    if (wasActive) {
        ALOGV("%s: Restarting activity to reconfigure streams", __FUNCTION__);
        res = configureStreamsLocked();
        if (res != OK) {
            ALOGE("%s: Can't reconfigure device for new stream %d: %s (%d)",
                    __FUNCTION__, mNextStreamId, strerror(-res), res);
            return res;
        }
        mRequestThread->setPaused(false);
    }

    return OK;
}

status_t Camera3Device::createStream(sp<ANativeWindow> consumer,
        uint32_t width, uint32_t height, int format, size_t size, int *id) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    status_t res;
    bool wasActive = false;

    switch (mStatus) {
        case STATUS_ERROR:
            ALOGE("%s: Device has encountered a serious error", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            ALOGE("%s: Device not initialized", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_IDLE:
            // OK
            break;
        case STATUS_ACTIVE:
            ALOGV("%s: Stopping activity to reconfigure streams", __FUNCTION__);
            mRequestThread->setPaused(true);
            res = waitUntilDrainedLocked();
            if (res != OK) {
                ALOGE("%s: Can't pause captures to reconfigure streams!",
                        __FUNCTION__);
                mStatus = STATUS_ERROR;
                return res;
            }
            wasActive = true;
            break;
        default:
            ALOGE("%s: Unexpected status: %d", __FUNCTION__, mStatus);
            return INVALID_OPERATION;
    }
    assert(mStatus == STATUS_IDLE);

    sp<Camera3OutputStream> newStream;
    if (format == HAL_PIXEL_FORMAT_BLOB) {
        newStream = new Camera3OutputStream(mNextStreamId, consumer,
                width, height, size, format);
    } else {
        newStream = new Camera3OutputStream(mNextStreamId, consumer,
                width, height, format);
    }

    res = mOutputStreams.add(mNextStreamId, newStream);
    if (res < 0) {
        ALOGE("%s: Can't add new stream to set: %s (%d)",
                __FUNCTION__, strerror(-res), res);
        return res;
    }

    *id = mNextStreamId++;

    // Continue captures if active at start
    if (wasActive) {
        ALOGV("%s: Restarting activity to reconfigure streams", __FUNCTION__);
        res = configureStreamsLocked();
        if (res != OK) {
            ALOGE("%s: Can't reconfigure device for new stream %d: %s (%d)",
                    __FUNCTION__, mNextStreamId, strerror(-res), res);
            return res;
        }
        mRequestThread->setPaused(false);
    }

    return OK;
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
    Mutex::Autolock l(mLock);

    switch (mStatus) {
        case STATUS_ERROR:
            ALOGE("%s: Device has encountered a serious error", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            ALOGE("%s: Device not initialized!", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_IDLE:
        case STATUS_ACTIVE:
            // OK
            break;
        default:
            ALOGE("%s: Unexpected status: %d", __FUNCTION__, mStatus);
            return INVALID_OPERATION;
    }

    ssize_t idx = mOutputStreams.indexOfKey(id);
    if (idx == NAME_NOT_FOUND) {
        ALOGE("%s: Stream %d is unknown", __FUNCTION__, id);
        return idx;
    }

    if (width) *width  = mOutputStreams[idx]->getWidth();
    if (height) *height = mOutputStreams[idx]->getHeight();
    if (format) *format = mOutputStreams[idx]->getFormat();

    return OK;
}

status_t Camera3Device::setStreamTransform(int id,
        int transform) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    switch (mStatus) {
        case STATUS_ERROR:
            ALOGE("%s: Device has encountered a serious error", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            ALOGE("%s: Device not initialized", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_IDLE:
        case STATUS_ACTIVE:
            // OK
            break;
        default:
            ALOGE("%s: Unexpected status: %d", __FUNCTION__, mStatus);
            return INVALID_OPERATION;
    }

    ssize_t idx = mOutputStreams.indexOfKey(id);
    if (idx == NAME_NOT_FOUND) {
        ALOGE("%s: Stream %d does not exist",
                __FUNCTION__, id);
        return BAD_VALUE;
    }

    return mOutputStreams.editValueAt(idx)->setTransform(transform);
}

status_t Camera3Device::deleteStream(int id) {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);
    status_t res;

    // CameraDevice semantics require device to already be idle before
    // deleteStream is called, unlike for createStream.
    if (mStatus != STATUS_IDLE) {
        ALOGE("%s: Device not idle", __FUNCTION__);
        return INVALID_OPERATION;
    }

    sp<Camera3Stream> deletedStream;
    if (mInputStream != NULL && id == mInputStream->getId()) {
        deletedStream = mInputStream;
        mInputStream.clear();
    } else {
        ssize_t idx = mOutputStreams.indexOfKey(id);
        if (idx == NAME_NOT_FOUND) {
            ALOGE("%s: Stream %d does not exist",
                    __FUNCTION__, id);
            return BAD_VALUE;
        }
        deletedStream = mOutputStreams.editValueAt(idx);
        mOutputStreams.removeItem(id);
    }

    // Free up the stream endpoint so that it can be used by some other stream
    res = deletedStream->disconnect();
    if (res != OK) {
        ALOGE("%s: Can't disconnect deleted stream", __FUNCTION__);
        // fall through since we want to still list the stream as deleted.
    }
    mDeletedStreams.add(deletedStream);

    return res;
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
    Mutex::Autolock l(mLock);

    switch (mStatus) {
        case STATUS_ERROR:
            ALOGE("%s: Device has encountered a serious error", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_UNINITIALIZED:
            ALOGE("%s: Device is not initialized!", __FUNCTION__);
            return INVALID_OPERATION;
        case STATUS_IDLE:
        case STATUS_ACTIVE:
            // OK
            break;
        default:
            ALOGE("%s: Unexpected status: %d", __FUNCTION__, mStatus);
            return INVALID_OPERATION;
    }

    const camera_metadata_t *rawRequest;
    rawRequest = mHal3Device->ops->construct_default_request_settings(
        mHal3Device, templateId);
    if (rawRequest == NULL) return DEAD_OBJECT;
    *request = rawRequest;

    return OK;
}

status_t Camera3Device::waitUntilDrained() {
    ATRACE_CALL();
    Mutex::Autolock l(mLock);

    return waitUntilDrainedLocked();
}

status_t Camera3Device::waitUntilDrainedLocked() {
    ATRACE_CALL();
    status_t res;

    switch (mStatus) {
        case STATUS_UNINITIALIZED:
        case STATUS_IDLE:
            ALOGV("%s: Already idle", __FUNCTION__);
            return OK;
        case STATUS_ERROR:
        case STATUS_ACTIVE:
            // Need to shut down
            break;
        default:
            ALOGE("%s: Unexpected status: %d", __FUNCTION__, mStatus);
            return INVALID_OPERATION;
    }

    if (mRequestThread != NULL) {
        res = mRequestThread->waitUntilPaused(kShutdownTimeout);
        if (res != OK) {
            ALOGE("%s: Can't stop request thread in %f seconds!",
                    __FUNCTION__, kShutdownTimeout/1e9);
            mStatus = STATUS_ERROR;
            return res;
        }
    }
    if (mInputStream != NULL) {
        res = mInputStream->waitUntilIdle(kShutdownTimeout);
        if (res != OK) {
            ALOGE("%s: Can't idle input stream %d in %f seconds!",
                    __FUNCTION__, mInputStream->getId(), kShutdownTimeout/1e9);
            mStatus = STATUS_ERROR;
            return res;
        }
    }
    for (size_t i = 0; i < mOutputStreams.size(); i++) {
        res = mOutputStreams.editValueAt(i)->waitUntilIdle(kShutdownTimeout);
        if (res != OK) {
            ALOGE("%s: Can't idle output stream %d in %f seconds!",
                    __FUNCTION__, mOutputStreams.keyAt(i),
                    kShutdownTimeout/1e9);
            mStatus = STATUS_ERROR;
            return res;
        }
    }

    if (mStatus != STATUS_ERROR) {
        mStatus = STATUS_IDLE;
    }

    return OK;
}

status_t Camera3Device::setNotifyCallback(NotificationListener *listener) {
    ATRACE_CALL();
    Mutex::Autolock l(mOutputLock);

    if (listener != NULL && mListener != NULL) {
        ALOGW("%s: Replacing old callback listener", __FUNCTION__);
    }
    mListener = listener;

    return OK;
}

status_t Camera3Device::waitForNextFrame(nsecs_t timeout) {
    ATRACE_CALL();
    status_t res;
    Mutex::Autolock l(mOutputLock);

    while (mResultQueue.empty()) {
        res = mResultSignal.waitRelative(mOutputLock, timeout);
        if (res == TIMED_OUT) {
            return res;
        } else if (res != OK) {
            ALOGE("%s: Camera %d: Error waiting for frame: %s (%d)",
                    __FUNCTION__, mId, strerror(-res), res);
            return res;
        }
    }
    return OK;
}

status_t Camera3Device::getNextFrame(CameraMetadata *frame) {
    ATRACE_CALL();
    Mutex::Autolock l(mOutputLock);

    if (mResultQueue.empty()) {
        return NOT_ENOUGH_DATA;
    }

    CameraMetadata &result = *(mResultQueue.begin());
    frame->acquire(result);
    mResultQueue.erase(mResultQueue.begin());

    return OK;
}

status_t Camera3Device::triggerAutofocus(uint32_t id) {
    ATRACE_CALL();

    ALOGV("%s: Triggering autofocus, id %d", __FUNCTION__, id);
    // Mix-in this trigger into the next request and only the next request.
    RequestTrigger trigger[] = {
        {
            ANDROID_CONTROL_AF_TRIGGER,
            ANDROID_CONTROL_AF_TRIGGER_START
        },
        {
            ANDROID_CONTROL_AF_TRIGGER_ID,
            static_cast<int32_t>(id)
        },
    };

    return mRequestThread->queueTrigger(trigger,
                                        sizeof(trigger)/sizeof(trigger[0]));
}

status_t Camera3Device::triggerCancelAutofocus(uint32_t id) {
    ATRACE_CALL();

    ALOGV("%s: Triggering cancel autofocus, id %d", __FUNCTION__, id);
    // Mix-in this trigger into the next request and only the next request.
    RequestTrigger trigger[] = {
        {
            ANDROID_CONTROL_AF_TRIGGER,
            ANDROID_CONTROL_AF_TRIGGER_CANCEL
        },
        {
            ANDROID_CONTROL_AF_TRIGGER_ID,
            static_cast<int32_t>(id)
        },
    };

    return mRequestThread->queueTrigger(trigger,
                                        sizeof(trigger)/sizeof(trigger[0]));
}

status_t Camera3Device::triggerPrecaptureMetering(uint32_t id) {
    ATRACE_CALL();

    ALOGV("%s: Triggering precapture metering, id %d", __FUNCTION__, id);
    // Mix-in this trigger into the next request and only the next request.
    RequestTrigger trigger[] = {
        {
            ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
            ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START
        },
        {
            ANDROID_CONTROL_AE_PRECAPTURE_ID,
            static_cast<int32_t>(id)
        },
    };

    return mRequestThread->queueTrigger(trigger,
                                        sizeof(trigger)/sizeof(trigger[0]));
}

status_t Camera3Device::pushReprocessBuffer(int reprocessStreamId,
        buffer_handle_t *buffer, wp<BufferReleasedListener> listener) {
    ATRACE_CALL();
    (void)reprocessStreamId; (void)buffer; (void)listener;

    ALOGE("%s: Unimplemented", __FUNCTION__);
    return INVALID_OPERATION;
}

/**
 * Camera3Device private methods
 */

sp<Camera3Device::CaptureRequest> Camera3Device::createCaptureRequest(
        const CameraMetadata &request) {
    ATRACE_CALL();
    status_t res;

    sp<CaptureRequest> newRequest = new CaptureRequest;
    newRequest->mSettings = request;

    camera_metadata_entry_t inputStreams =
            newRequest->mSettings.find(ANDROID_REQUEST_INPUT_STREAMS);
    if (inputStreams.count > 0) {
        if (mInputStream == NULL ||
                mInputStream->getId() != inputStreams.data.u8[0]) {
            ALOGE("%s: Request references unknown input stream %d",
                    __FUNCTION__, inputStreams.data.u8[0]);
            return NULL;
        }
        // Lazy completion of stream configuration (allocation/registration)
        // on first use
        if (mInputStream->isConfiguring()) {
            res = mInputStream->finishConfiguration(mHal3Device);
            if (res != OK) {
                ALOGE("%s: Unable to finish configuring input stream %d:"
                        " %s (%d)",
                        __FUNCTION__, mInputStream->getId(),
                        strerror(-res), res);
                return NULL;
            }
        }

        newRequest->mInputStream = mInputStream;
        newRequest->mSettings.erase(ANDROID_REQUEST_INPUT_STREAMS);
    }

    camera_metadata_entry_t streams =
            newRequest->mSettings.find(ANDROID_REQUEST_OUTPUT_STREAMS);
    if (streams.count == 0) {
        ALOGE("%s: Zero output streams specified!", __FUNCTION__);
        return NULL;
    }

    for (size_t i = 0; i < streams.count; i++) {
        int idx = mOutputStreams.indexOfKey(streams.data.u8[i]);
        if (idx == NAME_NOT_FOUND) {
            ALOGE("%s: Request references unknown stream %d",
                    __FUNCTION__, streams.data.u8[i]);
            return NULL;
        }
        sp<Camera3OutputStream> stream = mOutputStreams.editValueAt(idx);

        // Lazy completion of stream configuration (allocation/registration)
        // on first use
        if (stream->isConfiguring()) {
            res = stream->finishConfiguration(mHal3Device);
            if (res != OK) {
                ALOGE("%s: Unable to finish configuring stream %d: %s (%d)",
                        __FUNCTION__, stream->getId(), strerror(-res), res);
                return NULL;
            }
        }

        newRequest->mOutputStreams.push(stream);
    }
    newRequest->mSettings.erase(ANDROID_REQUEST_OUTPUT_STREAMS);

    return newRequest;
}

status_t Camera3Device::configureStreamsLocked() {
    ATRACE_CALL();
    status_t res;

    if (mStatus != STATUS_IDLE) {
        ALOGE("%s: Not idle", __FUNCTION__);
        return INVALID_OPERATION;
    }

    // Start configuring the streams

    camera3_stream_configuration config;

    config.num_streams = (mInputStream != NULL) + mOutputStreams.size();

    Vector<camera3_stream_t*> streams;
    streams.setCapacity(config.num_streams);

    if (mInputStream != NULL) {
        camera3_stream_t *inputStream;
        inputStream = mInputStream->startConfiguration();
        if (inputStream == NULL) {
            ALOGE("%s: Can't start input stream configuration",
                    __FUNCTION__);
            // TODO: Make sure the error flow here is correct
            return INVALID_OPERATION;
        }
        streams.add(inputStream);
    }

    for (size_t i = 0; i < mOutputStreams.size(); i++) {
        camera3_stream_t *outputStream;
        outputStream = mOutputStreams.editValueAt(i)->startConfiguration();
        if (outputStream == NULL) {
            ALOGE("%s: Can't start output stream configuration",
                    __FUNCTION__);
            // TODO: Make sure the error flow here is correct
            return INVALID_OPERATION;
        }
        streams.add(outputStream);
    }

    config.streams = streams.editArray();

    // Do the HAL configuration; will potentially touch stream
    // max_buffers, usage, priv fields.

    res = mHal3Device->ops->configure_streams(mHal3Device, &config);

    if (res != OK) {
        ALOGE("%s: Unable to configure streams with HAL: %s (%d)",
                __FUNCTION__, strerror(-res), res);
        return res;
    }

    // Request thread needs to know to avoid using repeat-last-settings protocol
    // across configure_streams() calls
    mRequestThread->configurationComplete();

    // Finish configuring the streams lazily on first reference

    mStatus = STATUS_ACTIVE;

    return OK;
}


/**
 * Camera HAL device callback methods
 */

void Camera3Device::processCaptureResult(const camera3_capture_result *result) {
    ATRACE_CALL();

    status_t res;

    if (result->result == NULL) {
        // TODO: Report error upstream
        ALOGW("%s: No metadata for frame %d", __FUNCTION__,
                result->frame_number);
        return;
    }

    nsecs_t timestamp = 0;
    AlgState cur3aState;
    AlgState new3aState;
    int32_t aeTriggerId = 0;
    int32_t afTriggerId = 0;

    NotificationListener *listener;

    {
        Mutex::Autolock l(mOutputLock);

        // Push result metadata into queue
        mResultQueue.push_back(CameraMetadata());
        // Lets avoid copies! Too bad there's not a #back method
        CameraMetadata &captureResult = *(--mResultQueue.end());

        captureResult = result->result;
        if (captureResult.update(ANDROID_REQUEST_FRAME_COUNT,
                (int32_t*)&result->frame_number, 1) != OK) {
            ALOGE("%s: Camera %d: Failed to set frame# in metadata (%d)",
                  __FUNCTION__, mId, result->frame_number);
            // TODO: Report error upstream
        } else {
            ALOGVV("%s: Camera %d: Set frame# in metadata (%d)",
                  __FUNCTION__, mId, result->frame_number);
        }

        // Get timestamp from result metadata

        camera_metadata_entry entry =
                captureResult.find(ANDROID_SENSOR_TIMESTAMP);
        if (entry.count == 0) {
            ALOGE("%s: Camera %d: No timestamp provided by HAL for frame %d!",
                    __FUNCTION__, mId, result->frame_number);
            // TODO: Report error upstream
        } else {
            timestamp = entry.data.i64[0];
        }

        // Get 3A states from result metadata

        entry = captureResult.find(ANDROID_CONTROL_AE_STATE);
        if (entry.count == 0) {
            ALOGE("%s: Camera %d: No AE state provided by HAL for frame %d!",
                    __FUNCTION__, mId, result->frame_number);
        } else {
            new3aState.aeState =
                    static_cast<camera_metadata_enum_android_control_ae_state>(
                        entry.data.u8[0]);
        }

        entry = captureResult.find(ANDROID_CONTROL_AF_STATE);
        if (entry.count == 0) {
            ALOGE("%s: Camera %d: No AF state provided by HAL for frame %d!",
                    __FUNCTION__, mId, result->frame_number);
        } else {
            new3aState.afState =
                    static_cast<camera_metadata_enum_android_control_af_state>(
                        entry.data.u8[0]);
        }

        entry = captureResult.find(ANDROID_CONTROL_AWB_STATE);
        if (entry.count == 0) {
            ALOGE("%s: Camera %d: No AWB state provided by HAL for frame %d!",
                    __FUNCTION__, mId, result->frame_number);
        } else {
            new3aState.awbState =
                    static_cast<camera_metadata_enum_android_control_awb_state>(
                        entry.data.u8[0]);
        }

        entry = captureResult.find(ANDROID_CONTROL_AF_TRIGGER_ID);
        if (entry.count == 0) {
            ALOGE("%s: Camera %d: No AF trigger ID provided by HAL for frame %d!",
                    __FUNCTION__, mId, result->frame_number);
        } else {
            afTriggerId = entry.data.i32[0];
        }

        entry = captureResult.find(ANDROID_CONTROL_AE_PRECAPTURE_ID);
        if (entry.count == 0) {
            ALOGE("%s: Camera %d: No AE precapture trigger ID provided by HAL"
                    " for frame %d!", __FUNCTION__, mId, result->frame_number);
        } else {
            aeTriggerId = entry.data.i32[0];
        }

        listener = mListener;
        cur3aState = m3AState;

        m3AState = new3aState;
    } // scope for mOutputLock

    // Return completed buffers to their streams
    for (size_t i = 0; i < result->num_output_buffers; i++) {
        Camera3Stream *stream =
                Camera3Stream::cast(result->output_buffers[i].stream);
        res = stream->returnBuffer(result->output_buffers[i], timestamp);
        // Note: stream may be deallocated at this point, if this buffer was the
        // last reference to it.
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't return buffer %d for frame %d to its"
                    "  stream:%s (%d)",  __FUNCTION__, mId, i,
                    result->frame_number, strerror(-res), res);
            // TODO: Report error upstream
        }
    }

    // Dispatch any 3A change events to listeners
    if (listener != NULL) {
        if (new3aState.aeState != cur3aState.aeState) {
            ALOGVV("%s: AE state changed from 0x%x to 0x%x",
                   __FUNCTION__, cur3aState.aeState, new3aState.aeState);
            listener->notifyAutoExposure(new3aState.aeState, aeTriggerId);
        }
        if (new3aState.afState != cur3aState.afState) {
            ALOGVV("%s: AF state changed from 0x%x to 0x%x",
                   __FUNCTION__, cur3aState.afState, new3aState.afState);
            listener->notifyAutoFocus(new3aState.afState, afTriggerId);
        }
        if (new3aState.awbState != cur3aState.awbState) {
            listener->notifyAutoWhitebalance(new3aState.awbState, aeTriggerId);
        }
    }

}

void Camera3Device::notify(const camera3_notify_msg *msg) {
    NotificationListener *listener;
    {
        Mutex::Autolock l(mOutputLock);
        if (mListener == NULL) return;
        listener = mListener;
    }

    if (msg == NULL) {
        ALOGE("%s: Camera %d: HAL sent NULL notify message!",
                __FUNCTION__, mId);
        return;
    }

    switch (msg->type) {
        case CAMERA3_MSG_ERROR: {
            int streamId = 0;
            if (msg->message.error.error_stream != NULL) {
                Camera3Stream *stream =
                        Camera3Stream::cast(
                                  msg->message.error.error_stream);
                streamId = stream->getId();
            }
            listener->notifyError(msg->message.error.error_code,
                    msg->message.error.frame_number, streamId);
            break;
        }
        case CAMERA3_MSG_SHUTTER: {
            listener->notifyShutter(msg->message.shutter.frame_number,
                    msg->message.shutter.timestamp);
            break;
        }
        default:
            ALOGE("%s: Camera %d: Unknown notify message from HAL: %d",
                    __FUNCTION__, mId, msg->type);
    }
}

/**
 * RequestThread inner class methods
 */

Camera3Device::RequestThread::RequestThread(wp<Camera3Device> parent,
        camera3_device_t *hal3Device) :
        Thread(false),
        mParent(parent),
        mHal3Device(hal3Device),
        mReconfigured(false),
        mDoPause(false),
        mPaused(true),
        mFrameNumber(0),
        mLatestRequestId(NAME_NOT_FOUND) {
}

void Camera3Device::RequestThread::configurationComplete() {
    Mutex::Autolock l(mRequestLock);
    mReconfigured = true;
}

status_t Camera3Device::RequestThread::queueRequest(
         sp<CaptureRequest> request) {
    Mutex::Autolock l(mRequestLock);
    mRequestQueue.push_back(request);

    return OK;
}


status_t Camera3Device::RequestThread::queueTrigger(
        RequestTrigger trigger[],
        size_t count) {

    Mutex::Autolock l(mTriggerMutex);
    status_t ret;

    for (size_t i = 0; i < count; ++i) {
        ret = queueTriggerLocked(trigger[i]);

        if (ret != OK) {
            return ret;
        }
    }

    return OK;
}

status_t Camera3Device::RequestThread::queueTriggerLocked(
        RequestTrigger trigger) {

    uint32_t tag = trigger.metadataTag;
    ssize_t index = mTriggerMap.indexOfKey(tag);

    switch (trigger.getTagType()) {
        case TYPE_BYTE:
        // fall-through
        case TYPE_INT32:
            break;
        default:
            ALOGE("%s: Type not supported: 0x%x",
                  __FUNCTION__,
                  trigger.getTagType());
            return INVALID_OPERATION;
    }

    /**
     * Collect only the latest trigger, since we only have 1 field
     * in the request settings per trigger tag, and can't send more than 1
     * trigger per request.
     */
    if (index != NAME_NOT_FOUND) {
        mTriggerMap.editValueAt(index) = trigger;
    } else {
        mTriggerMap.add(tag, trigger);
    }

    return OK;
}

status_t Camera3Device::RequestThread::setRepeatingRequests(
        const RequestList &requests) {
    Mutex::Autolock l(mRequestLock);
    mRepeatingRequests.clear();
    mRepeatingRequests.insert(mRepeatingRequests.begin(),
            requests.begin(), requests.end());
    return OK;
}

status_t Camera3Device::RequestThread::clearRepeatingRequests() {
    Mutex::Autolock l(mRequestLock);
    mRepeatingRequests.clear();
    return OK;
}

void Camera3Device::RequestThread::setPaused(bool paused) {
    Mutex::Autolock l(mPauseLock);
    mDoPause = paused;
    mDoPauseSignal.signal();
}

status_t Camera3Device::RequestThread::waitUntilPaused(nsecs_t timeout) {
    status_t res;
    Mutex::Autolock l(mPauseLock);
    while (!mPaused) {
        res = mPausedSignal.waitRelative(mPauseLock, timeout);
        if (res == TIMED_OUT) {
            return res;
        }
    }
    return OK;
}

status_t Camera3Device::RequestThread::waitUntilRequestProcessed(
        int32_t requestId, nsecs_t timeout) {
    Mutex::Autolock l(mLatestRequestMutex);
    status_t res;
    while (mLatestRequestId != requestId) {
        nsecs_t startTime = systemTime();

        res = mLatestRequestSignal.waitRelative(mLatestRequestMutex, timeout);
        if (res != OK) return res;

        timeout -= (systemTime() - startTime);
    }

    return OK;
}



bool Camera3Device::RequestThread::threadLoop() {

    status_t res;

    // Handle paused state.
    if (waitIfPaused()) {
        return true;
    }

    // Get work to do

    sp<CaptureRequest> nextRequest = waitForNextRequest();
    if (nextRequest == NULL) {
        return true;
    }

    // Create request to HAL
    camera3_capture_request_t request = camera3_capture_request_t();
    Vector<camera3_stream_buffer_t> outputBuffers;

    // Insert any queued triggers (before metadata is locked)
    int32_t triggerCount;
    res = insertTriggers(nextRequest);
    if (res < 0) {
        ALOGE("RequestThread: Unable to insert triggers "
              "(capture request %d, HAL device: %s (%d)",
              (mFrameNumber+1), strerror(-res), res);
        cleanUpFailedRequest(request, nextRequest, outputBuffers);
        return false;
    }
    triggerCount = res;

    bool triggersMixedIn = (triggerCount > 0 || mPrevTriggers > 0);

    // If the request is the same as last, or we had triggers last time
    if (mPrevRequest != nextRequest || triggersMixedIn) {
        /**
         * The request should be presorted so accesses in HAL
         *   are O(logn). Sidenote, sorting a sorted metadata is nop.
         */
        nextRequest->mSettings.sort();
        request.settings = nextRequest->mSettings.getAndLock();
        mPrevRequest = nextRequest;
        ALOGVV("%s: Request settings are NEW", __FUNCTION__);

        IF_ALOGV() {
            camera_metadata_ro_entry_t e = camera_metadata_ro_entry_t();
            find_camera_metadata_ro_entry(
                    request.settings,
                    ANDROID_CONTROL_AF_TRIGGER,
                    &e
            );
            if (e.count > 0) {
                ALOGV("%s: Request (frame num %d) had AF trigger 0x%x",
                      __FUNCTION__,
                      mFrameNumber+1,
                      e.data.u8[0]);
            }
        }
    } else {
        // leave request.settings NULL to indicate 'reuse latest given'
        ALOGVV("%s: Request settings are REUSED",
               __FUNCTION__);
    }

    camera3_stream_buffer_t inputBuffer;

    // Fill in buffers

    if (nextRequest->mInputStream != NULL) {
        request.input_buffer = &inputBuffer;
        res = nextRequest->mInputStream->getInputBuffer(&inputBuffer);
        if (res != OK) {
            ALOGE("RequestThread: Can't get input buffer, skipping request:"
                    " %s (%d)", strerror(-res), res);
            cleanUpFailedRequest(request, nextRequest, outputBuffers);
            return true;
        }
    } else {
        request.input_buffer = NULL;
    }

    outputBuffers.insertAt(camera3_stream_buffer_t(), 0,
            nextRequest->mOutputStreams.size());
    request.output_buffers = outputBuffers.array();
    for (size_t i = 0; i < nextRequest->mOutputStreams.size(); i++) {
        res = nextRequest->mOutputStreams.editItemAt(i)->
                getBuffer(&outputBuffers.editItemAt(i));
        if (res != OK) {
            ALOGE("RequestThread: Can't get output buffer, skipping request:"
                    "%s (%d)", strerror(-res), res);
            cleanUpFailedRequest(request, nextRequest, outputBuffers);
            return true;
        }
        request.num_output_buffers++;
    }

    request.frame_number = mFrameNumber++;


    // Submit request and block until ready for next one

    res = mHal3Device->ops->process_capture_request(mHal3Device, &request);
    if (res != OK) {
        ALOGE("RequestThread: Unable to submit capture request %d to HAL"
                " device: %s (%d)", request.frame_number, strerror(-res), res);
        cleanUpFailedRequest(request, nextRequest, outputBuffers);
        return false;
    }

    if (request.settings != NULL) {
        nextRequest->mSettings.unlock(request.settings);
    }

    // Remove any previously queued triggers (after unlock)
    res = removeTriggers(mPrevRequest);
    if (res != OK) {
        ALOGE("RequestThread: Unable to remove triggers "
              "(capture request %d, HAL device: %s (%d)",
              request.frame_number, strerror(-res), res);
        return false;
    }
    mPrevTriggers = triggerCount;

    // Read android.request.id from the request settings metadata
    // - inform waitUntilRequestProcessed thread of a new request ID
    {
        Mutex::Autolock al(mLatestRequestMutex);

        camera_metadata_entry_t requestIdEntry =
                nextRequest->mSettings.find(ANDROID_REQUEST_ID);
        if (requestIdEntry.count > 0) {
            mLatestRequestId = requestIdEntry.data.i32[0];
        } else {
            ALOGW("%s: Did not have android.request.id set in the request",
                  __FUNCTION__);
            mLatestRequestId = NAME_NOT_FOUND;
        }

        mLatestRequestSignal.signal();
    }

    // Return input buffer back to framework
    if (request.input_buffer != NULL) {
        Camera3Stream *stream =
            Camera3Stream::cast(request.input_buffer->stream);
        res = stream->returnInputBuffer(*(request.input_buffer));
        // Note: stream may be deallocated at this point, if this buffer was the
        // last reference to it.
        if (res != OK) {
            ALOGE("%s: RequestThread: Can't return input buffer for frame %d to"
                    "  its stream:%s (%d)",  __FUNCTION__,
                    request.frame_number, strerror(-res), res);
            // TODO: Report error upstream
        }
    }



    return true;
}

void Camera3Device::RequestThread::cleanUpFailedRequest(
        camera3_capture_request_t &request,
        sp<CaptureRequest> &nextRequest,
        Vector<camera3_stream_buffer_t> &outputBuffers) {

    if (request.settings != NULL) {
        nextRequest->mSettings.unlock(request.settings);
    }
    if (request.input_buffer != NULL) {
        request.input_buffer->status = CAMERA3_BUFFER_STATUS_ERROR;
        nextRequest->mInputStream->returnInputBuffer(*(request.input_buffer));
    }
    for (size_t i = 0; i < request.num_output_buffers; i++) {
        outputBuffers.editItemAt(i).status = CAMERA3_BUFFER_STATUS_ERROR;
        nextRequest->mOutputStreams.editItemAt(i)->returnBuffer(
            outputBuffers[i], 0);
    }
    // TODO: Report error upstream
}

sp<Camera3Device::CaptureRequest>
        Camera3Device::RequestThread::waitForNextRequest() {
    status_t res;
    sp<CaptureRequest> nextRequest;

    // Optimized a bit for the simple steady-state case (single repeating
    // request), to avoid putting that request in the queue temporarily.
    Mutex::Autolock l(mRequestLock);

    while (mRequestQueue.empty()) {
        if (!mRepeatingRequests.empty()) {
            // Always atomically enqueue all requests in a repeating request
            // list. Guarantees a complete in-sequence set of captures to
            // application.
            const RequestList &requests = mRepeatingRequests;
            RequestList::const_iterator firstRequest =
                    requests.begin();
            nextRequest = *firstRequest;
            mRequestQueue.insert(mRequestQueue.end(),
                    ++firstRequest,
                    requests.end());
            // No need to wait any longer
            break;
        }

        res = mRequestSignal.waitRelative(mRequestLock, kRequestTimeout);

        if (res == TIMED_OUT) {
            // Signal that we're paused by starvation
            Mutex::Autolock pl(mPauseLock);
            if (mPaused == false) {
                mPaused = true;
                mPausedSignal.signal();
            }
            // Stop waiting for now and let thread management happen
            return NULL;
        }
    }

    if (nextRequest == NULL) {
        // Don't have a repeating request already in hand, so queue
        // must have an entry now.
        RequestList::iterator firstRequest =
                mRequestQueue.begin();
        nextRequest = *firstRequest;
        mRequestQueue.erase(firstRequest);
    }

    // Not paused
    Mutex::Autolock pl(mPauseLock);
    mPaused = false;

    // Check if we've reconfigured since last time, and reset the preview
    // request if so. Can't use 'NULL request == repeat' across configure calls.
    if (mReconfigured) {
        mPrevRequest.clear();
        mReconfigured = false;
    }

    return nextRequest;
}

bool Camera3Device::RequestThread::waitIfPaused() {
    status_t res;
    Mutex::Autolock l(mPauseLock);
    while (mDoPause) {
        // Signal that we're paused by request
        if (mPaused == false) {
            mPaused = true;
            mPausedSignal.signal();
        }
        res = mDoPauseSignal.waitRelative(mPauseLock, kRequestTimeout);
        if (res == TIMED_OUT) {
            return true;
        }
    }
    // We don't set mPaused to false here, because waitForNextRequest needs
    // to further manage the paused state in case of starvation.
    return false;
}

status_t Camera3Device::RequestThread::insertTriggers(
        const sp<CaptureRequest> &request) {

    Mutex::Autolock al(mTriggerMutex);

    CameraMetadata &metadata = request->mSettings;
    size_t count = mTriggerMap.size();

    for (size_t i = 0; i < count; ++i) {
        RequestTrigger trigger = mTriggerMap.valueAt(i);

        uint32_t tag = trigger.metadataTag;
        camera_metadata_entry entry = metadata.find(tag);

        if (entry.count > 0) {
            /**
             * Already has an entry for this trigger in the request.
             * Rewrite it with our requested trigger value.
             */
            RequestTrigger oldTrigger = trigger;

            oldTrigger.entryValue = entry.data.u8[0];

            mTriggerReplacedMap.add(tag, oldTrigger);
        } else {
            /**
             * More typical, no trigger entry, so we just add it
             */
            mTriggerRemovedMap.add(tag, trigger);
        }

        status_t res;

        switch (trigger.getTagType()) {
            case TYPE_BYTE: {
                uint8_t entryValue = static_cast<uint8_t>(trigger.entryValue);
                res = metadata.update(tag,
                                      &entryValue,
                                      /*count*/1);
                break;
            }
            case TYPE_INT32:
                res = metadata.update(tag,
                                      &trigger.entryValue,
                                      /*count*/1);
                break;
            default:
                ALOGE("%s: Type not supported: 0x%x",
                      __FUNCTION__,
                      trigger.getTagType());
                return INVALID_OPERATION;
        }

        if (res != OK) {
            ALOGE("%s: Failed to update request metadata with trigger tag %s"
                  ", value %d", __FUNCTION__, trigger.getTagName(),
                  trigger.entryValue);
            return res;
        }

        ALOGV("%s: Mixed in trigger %s, value %d", __FUNCTION__,
              trigger.getTagName(),
              trigger.entryValue);
    }

    mTriggerMap.clear();

    return count;
}

status_t Camera3Device::RequestThread::removeTriggers(
        const sp<CaptureRequest> &request) {
    Mutex::Autolock al(mTriggerMutex);

    CameraMetadata &metadata = request->mSettings;

    /**
     * Replace all old entries with their old values.
     */
    for (size_t i = 0; i < mTriggerReplacedMap.size(); ++i) {
        RequestTrigger trigger = mTriggerReplacedMap.valueAt(i);

        status_t res;

        uint32_t tag = trigger.metadataTag;
        switch (trigger.getTagType()) {
            case TYPE_BYTE: {
                uint8_t entryValue = static_cast<uint8_t>(trigger.entryValue);
                res = metadata.update(tag,
                                      &entryValue,
                                      /*count*/1);
                break;
            }
            case TYPE_INT32:
                res = metadata.update(tag,
                                      &trigger.entryValue,
                                      /*count*/1);
                break;
            default:
                ALOGE("%s: Type not supported: 0x%x",
                      __FUNCTION__,
                      trigger.getTagType());
                return INVALID_OPERATION;
        }

        if (res != OK) {
            ALOGE("%s: Failed to restore request metadata with trigger tag %s"
                  ", trigger value %d", __FUNCTION__,
                  trigger.getTagName(), trigger.entryValue);
            return res;
        }
    }
    mTriggerReplacedMap.clear();

    /**
     * Remove all new entries.
     */
    for (size_t i = 0; i < mTriggerRemovedMap.size(); ++i) {
        RequestTrigger trigger = mTriggerRemovedMap.valueAt(i);
        status_t res = metadata.erase(trigger.metadataTag);

        if (res != OK) {
            ALOGE("%s: Failed to erase metadata with trigger tag %s"
                  ", trigger value %d", __FUNCTION__,
                  trigger.getTagName(), trigger.entryValue);
            return res;
        }
    }
    mTriggerRemovedMap.clear();

    return OK;
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
