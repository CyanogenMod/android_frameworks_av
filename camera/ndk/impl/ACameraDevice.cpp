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

//#define LOG_NDEBUG 0
#define LOG_TAG "ACameraDevice"

#include <inttypes.h>
#include "ACameraDevice.h"
#include "ACameraMetadata.h"
#include "ACaptureRequest.h"

using namespace android;

namespace android {
// Static member definitions
const char* CameraDevice::kContextKey   = "Context";
const char* CameraDevice::kDeviceKey    = "Device";
const char* CameraDevice::kErrorCodeKey = "ErrorCode";
const char* CameraDevice::kCallbackKey  = "Callback";

/**
 * CameraDevice Implementation
 */
CameraDevice::CameraDevice(
        const char* id,
        ACameraDevice_StateCallbacks* cb,
        std::unique_ptr<ACameraMetadata> chars,
        ACameraDevice* wrapper) :
        mCameraId(id),
        mAppCallbacks(*cb),
        mChars(std::move(chars)),
        mServiceCallback(new ServiceCallback(this)),
        mWrapper(wrapper),
        mInError(false),
        mError(ACAMERA_OK),
        mIdle(true) {
    mClosing = false;
    // Setup looper thread to perfrom device callbacks to app
    mCbLooper = new ALooper;
    mCbLooper->setName("C2N-dev-looper");
    status_t ret = mCbLooper->start(
            /*runOnCallingThread*/false,
            /*canCallJava*/       true,
            PRIORITY_FOREGROUND);
    mHandler = new CallbackHandler();
    mCbLooper->registerHandler(mHandler);
}

CameraDevice::~CameraDevice() {
    Mutex::Autolock _l(mDeviceLock);
    if (mCbLooper != nullptr) {
        mCbLooper->unregisterHandler(mHandler->id());
        mCbLooper->stop();
    }
    mCbLooper.clear();
    mHandler.clear();
    if (!isClosed()) {
        disconnectLocked();
    }
}

// TODO: cached created request?
camera_status_t
CameraDevice::createCaptureRequest(
        ACameraDevice_request_template templateId,
        ACaptureRequest** request) const {
    Mutex::Autolock _l(mDeviceLock);
    camera_status_t ret = checkCameraClosedOrErrorLocked();
    if (ret != ACAMERA_OK) {
        return ret;
    }
    if (mRemote == nullptr) {
        return ACAMERA_ERROR_CAMERA_DISCONNECTED;
    }
    CameraMetadata rawRequest;
    status_t remoteRet = mRemote->createDefaultRequest(templateId, &rawRequest);
    if (remoteRet == BAD_VALUE) {
        ALOGW("Create capture request failed! template %d is not supported on this device",
            templateId);
        return ACAMERA_ERROR_UNSUPPORTED;
    } else if (remoteRet != OK) {
        ALOGE("Create capture request failed! error %d", remoteRet);
        return ACAMERA_ERROR_UNKNOWN;
    }
    ACaptureRequest* outReq = new ACaptureRequest();
    outReq->settings = new ACameraMetadata(rawRequest.release(), ACameraMetadata::ACM_REQUEST);
    outReq->targets  = new ACameraOutputTargets();
    *request = outReq;
    return ACAMERA_OK;
}

void
CameraDevice::disconnectLocked() {
    if (mClosing.exchange(true)) {
        // Already closing, just return
        ALOGW("Camera device %s is already closing.", getId());
        return;
    }

    if (mRemote != nullptr) {
        mRemote->disconnect();
    }
    mRemote = nullptr;
}

void
CameraDevice::setRemoteDevice(sp<ICameraDeviceUser> remote) {
    Mutex::Autolock _l(mDeviceLock);
    mRemote = remote;
}

camera_status_t
CameraDevice::checkCameraClosedOrErrorLocked() const {
    if (mRemote == nullptr) {
        ALOGE("%s: camera device already closed", __FUNCTION__);
        return ACAMERA_ERROR_CAMERA_DISCONNECTED;
    }
    if (mInError) {// triggered by onDeviceError
        ALOGE("%s: camera device has encountered a serious error", __FUNCTION__);
        return mError;
    }
    return ACAMERA_OK;
}

void
CameraDevice::onCaptureErrorLocked(
        ICameraDeviceCallbacks::CameraErrorCode errorCode,
        const CaptureResultExtras& resultExtras) {
    // TODO: implement!
}

void CameraDevice::CallbackHandler::onMessageReceived(
        const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatOnDisconnected:
        case kWhatOnError:
            break;
        default:
            ALOGE("%s:Error: unknown device callback %d", __FUNCTION__, msg->what());
            return;
    }
    // Check the common part of all message
    void* context;
    bool found = msg->findPointer(kContextKey, &context);
    if (!found) {
        ALOGE("%s: Cannot find callback context!", __FUNCTION__);
        return;
    }
    ACameraDevice* dev;
    found = msg->findPointer(kDeviceKey, (void**) &dev);
    if (!found) {
        ALOGE("%s: Cannot find device pointer!", __FUNCTION__);
        return;
    }
    switch (msg->what()) {
        case kWhatOnDisconnected:
        {
            ACameraDevice_StateCallback onDisconnected;
            found = msg->findPointer(kCallbackKey, (void**) &onDisconnected);
            if (!found) {
                ALOGE("%s: Cannot find onDisconnected!", __FUNCTION__);
                return;
            }
            (*onDisconnected)(context, dev);
            break;
        }
        case kWhatOnError:
        {
            ACameraDevice_ErrorStateCallback onError;
            found = msg->findPointer(kCallbackKey, (void**) &onError);
            if (!found) {
                ALOGE("%s: Cannot find onError!", __FUNCTION__);
                return;
            }
            int errorCode;
            found = msg->findInt32(kErrorCodeKey, &errorCode);
            if (!found) {
                ALOGE("%s: Cannot find error code!", __FUNCTION__);
                return;
            }
            (*onError)(context, dev, errorCode);
        }
    }
}

/**
  * Camera service callback implementation
  */
void
CameraDevice::ServiceCallback::onDeviceError(
        CameraErrorCode errorCode,
        const CaptureResultExtras& resultExtras) {
    ALOGD("Device error received, code %d, frame number %" PRId64 ", request ID %d, subseq ID %d",
            errorCode, resultExtras.frameNumber, resultExtras.requestId, resultExtras.burstId);

    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return; // device has been closed
    }

    Mutex::Autolock _l(dev->mDeviceLock);
    if (dev->mRemote == nullptr) {
        return; // device has been disconnected
    }
    switch (errorCode) {
        case ERROR_CAMERA_DISCONNECTED:
        {
            // should be clear mRemote here?
            // TODO: close current session
            sp<AMessage> msg = new AMessage(kWhatOnDisconnected, dev->mHandler);
            msg->setPointer(kContextKey, dev->mAppCallbacks.context);
            msg->setPointer(kDeviceKey, (void*) dev->getWrapper());
            msg->setPointer(kCallbackKey, (void*) dev->mAppCallbacks.onDisconnected);
            msg->post();
            break;
        }
        default:
            ALOGE("Unknown error from camera device: %d", errorCode);
            // no break
        case ERROR_CAMERA_DEVICE:
        case ERROR_CAMERA_SERVICE:
        {
            dev->mInError = true;
            switch (errorCode) {
                case ERROR_CAMERA_DEVICE:
                    dev->mError = ACAMERA_ERROR_CAMERA_DEVICE;
                    break;
                case ERROR_CAMERA_SERVICE:
                    dev->mError = ACAMERA_ERROR_CAMERA_SERVICE;
                    break;
                default:
                    dev->mError = ACAMERA_ERROR_UNKNOWN;
                    break;
            }
            sp<AMessage> msg = new AMessage(kWhatOnError, dev->mHandler);
            msg->setPointer(kContextKey, dev->mAppCallbacks.context);
            msg->setPointer(kDeviceKey, (void*) dev->getWrapper());
            msg->setPointer(kCallbackKey, (void*) dev->mAppCallbacks.onError);
            msg->setInt32(kErrorCodeKey, errorCode);
            msg->post();
            break;
        }
        case ERROR_CAMERA_REQUEST:
        case ERROR_CAMERA_RESULT:
        case ERROR_CAMERA_BUFFER:
            dev->onCaptureErrorLocked(errorCode, resultExtras);
            break;
    }
}

void
CameraDevice::ServiceCallback::onDeviceIdle() {
    ALOGV("Camera is now idle");
    sp<CameraDevice> dev = mDevice.promote();
    if (dev == nullptr) {
        return; // device has been closed
    }

    Mutex::Autolock _l(dev->mDeviceLock);
    if (dev->mRemote == nullptr) {
        return; // device has been disconnected
    }
    if (!dev->mIdle) {
        // TODO: send idle callback to current session
    }
    dev->mIdle = true;
}

void
CameraDevice::ServiceCallback::onCaptureStarted(
        const CaptureResultExtras& resultExtras,
        int64_t timestamp) {
}

void
CameraDevice::ServiceCallback::onResultReceived(
        const CameraMetadata& metadata,
        const CaptureResultExtras& resultExtras) {
}

void
CameraDevice::ServiceCallback::onPrepared(int) {
    // Prepare not yet implemented in NDK
    return;
}

} // namespace android
