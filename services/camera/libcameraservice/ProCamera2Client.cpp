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

#define LOG_TAG "ProCamera2Client"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>

#include <cutils/properties.h>
#include <gui/Surface.h>
#include <gui/Surface.h>
#include "camera2/Parameters.h"
#include "ProCamera2Client.h"
#include "camera2/ProFrameProcessor.h"

namespace android {
using namespace camera2;

static int getCallingPid() {
    return IPCThreadState::self()->getCallingPid();
}

static int getCallingUid() {
    return IPCThreadState::self()->getCallingUid();
}

// Interface used by CameraService

ProCamera2Client::ProCamera2Client(const sp<CameraService>& cameraService,
        const sp<IProCameraCallbacks>& remoteCallback,
        int cameraId,
        int cameraFacing,
        int clientPid,
        int servicePid):
        ProClient(cameraService, remoteCallback,
                cameraId, cameraFacing, clientPid, servicePid),
        mSharedCameraCallbacks(remoteCallback)
{
    ATRACE_CALL();
    ALOGI("ProCamera %d: Opened", cameraId);

    mDevice = new Camera2Device(cameraId);

    mExclusiveLock = false;
}

status_t ProCamera2Client::checkPid(const char* checkLocation) const {
    int callingPid = getCallingPid();
    if (callingPid == mClientPid) return NO_ERROR;

    ALOGE("%s: attempt to use a locked camera from a different process"
            " (old pid %d, new pid %d)", checkLocation, mClientPid, callingPid);
    return PERMISSION_DENIED;
}

status_t ProCamera2Client::initialize(camera_module_t *module)
{
    ATRACE_CALL();
    ALOGV("%s: Initializing client for camera %d", __FUNCTION__, mCameraId);
    status_t res;

    res = mDevice->initialize(module);
    if (res != OK) {
        ALOGE("%s: Camera %d: unable to initialize device: %s (%d)",
                __FUNCTION__, mCameraId, strerror(-res), res);
        return NO_INIT;
    }

    res = mDevice->setNotifyCallback(this);

    String8 threadName;
    mFrameProcessor = new ProFrameProcessor(this);
    threadName = String8::format("PC2-%d-FrameProc",
            mCameraId);
    mFrameProcessor->run(threadName.string());

    mFrameProcessor->registerListener(FRAME_PROCESSOR_LISTENER_MIN_ID,
                                      FRAME_PROCESSOR_LISTENER_MAX_ID,
                                      /*listener*/this);

    return OK;
}

ProCamera2Client::~ProCamera2Client() {
    ATRACE_CALL();

    mDestructionStarted = true;

    disconnect();

    ALOGI("ProCamera %d: Closed", mCameraId);
}

status_t ProCamera2Client::exclusiveTryLock() {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    Mutex::Autolock icl(mIProCameraUserLock);
    SharedCameraCallbacks::Lock l(mSharedCameraCallbacks);

    if (!mExclusiveLock) {
        mExclusiveLock = true;

        if (mRemoteCallback != NULL) {
            mRemoteCallback->onLockStatusChanged(
                              IProCameraCallbacks::LOCK_ACQUIRED);
        }

        ALOGV("%s: exclusive lock acquired", __FUNCTION__);

        return OK;
    }

    // TODO: have a PERMISSION_DENIED case for when someone else owns the lock

    // don't allow recursive locking
    ALOGW("%s: exclusive lock already exists - recursive locking is not"
          "allowed", __FUNCTION__);

    return ALREADY_EXISTS;
}

status_t ProCamera2Client::exclusiveLock() {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    Mutex::Autolock icl(mIProCameraUserLock);
    SharedCameraCallbacks::Lock l(mSharedCameraCallbacks);

    /**
     * TODO: this should asynchronously 'wait' until the lock becomes available
     * if another client already has an exclusive lock.
     *
     * once we have proper sharing support this will need to do
     * more than just return immediately
     */
    if (!mExclusiveLock) {
        mExclusiveLock = true;

        if (mRemoteCallback != NULL) {
            mRemoteCallback->onLockStatusChanged(IProCameraCallbacks::LOCK_ACQUIRED);
        }

        ALOGV("%s: exclusive lock acquired", __FUNCTION__);

        return OK;
    }

    // don't allow recursive locking
    ALOGW("%s: exclusive lock already exists - recursive locking is not allowed"
                                                                , __FUNCTION__);
    return ALREADY_EXISTS;
}

status_t ProCamera2Client::exclusiveUnlock() {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    Mutex::Autolock icl(mIProCameraUserLock);
    SharedCameraCallbacks::Lock l(mSharedCameraCallbacks);

    // don't allow unlocking if we have no lock
    if (!mExclusiveLock) {
        ALOGW("%s: cannot unlock, no lock was held in the first place",
              __FUNCTION__);
        return BAD_VALUE;
    }

    mExclusiveLock = false;
    if (mRemoteCallback != NULL ) {
        mRemoteCallback->onLockStatusChanged(
                                       IProCameraCallbacks::LOCK_RELEASED);
    }
    ALOGV("%s: exclusive lock released", __FUNCTION__);

    return OK;
}

bool ProCamera2Client::hasExclusiveLock() {
    return mExclusiveLock;
}

status_t ProCamera2Client::submitRequest(camera_metadata_t* request,
                                         bool streaming) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    Mutex::Autolock icl(mIProCameraUserLock);
    if (!mExclusiveLock) {
        return PERMISSION_DENIED;
    }

    CameraMetadata metadata(request);

    if (streaming) {
        return mDevice->setStreamingRequest(metadata);
    } else {
        return mDevice->capture(metadata);
    }

    // unreachable. thx gcc for a useless warning
    return OK;
}

status_t ProCamera2Client::cancelRequest(int requestId) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    Mutex::Autolock icl(mIProCameraUserLock);
    if (!mExclusiveLock) {
        return PERMISSION_DENIED;
    }

    ALOGE("%s: not fully implemented yet", __FUNCTION__);
    return INVALID_OPERATION;
}

status_t ProCamera2Client::requestStream(int streamId) {
    ALOGE("%s: not implemented yet", __FUNCTION__);

    return INVALID_OPERATION;
}

status_t ProCamera2Client::cancelStream(int streamId) {
    ATRACE_CALL();
    ALOGV("%s (streamId = 0x%x)", __FUNCTION__, streamId);

    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mIProCameraUserLock);

    mDevice->clearStreamingRequest();

    status_t code;
    if ((code = mDevice->waitUntilDrained()) != OK) {
        ALOGE("%s: waitUntilDrained failed with code 0x%x", __FUNCTION__, code);
    }

    return mDevice->deleteStream(streamId);
}

status_t ProCamera2Client::createStream(int width, int height, int format,
                      const sp<IGraphicBufferProducer>& bufferProducer,
                      /*out*/
                      int* streamId)
{
    if (streamId) {
        *streamId = -1;
    }

    ATRACE_CALL();
    ALOGV("%s (w = %d, h = %d, f = 0x%x)", __FUNCTION__, width, height, format);

    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mIProCameraUserLock);

    sp<IBinder> binder;
    sp<ANativeWindow> window;
    if (bufferProducer != 0) {
        binder = bufferProducer->asBinder();
        window = new Surface(bufferProducer);
    }

    return mDevice->createStream(window, width, height, format, /*size*/1,
                                 streamId);
}

// Create a request object from a template.
// -- Caller owns the newly allocated metadata
status_t ProCamera2Client::createDefaultRequest(int templateId,
                             /*out*/
                              camera_metadata** request)
{
    ATRACE_CALL();
    ALOGV("%s (templateId = 0x%x)", __FUNCTION__, templateId);

    if (request) {
        *request = NULL;
    }

    status_t res;
    if ( (res = checkPid(__FUNCTION__) ) != OK) return res;

    Mutex::Autolock icl(mIProCameraUserLock);

    CameraMetadata metadata;
    if ( (res = mDevice->createDefaultRequest(templateId, &metadata) ) == OK) {
        *request = metadata.release();
    }

    return res;
}

status_t ProCamera2Client::dump(int fd, const Vector<String16>& args) {
    String8 result;
    result.appendFormat("ProCamera2Client[%d] (%p) PID: %d, dump:\n",
            mCameraId,
            getRemoteCallback()->asBinder().get(),
            mClientPid);
    result.append("  State: ");

    // TODO: print dynamic/request section from most recent requests
    mFrameProcessor->dump(fd, args);

#define CASE_APPEND_ENUM(x) case x: result.append(#x "\n"); break;

    result = "  Device dump:\n";
    write(fd, result.string(), result.size());

    status_t res = mDevice->dump(fd, args);
    if (res != OK) {
        result = String8::format("   Error dumping device: %s (%d)",
                strerror(-res), res);
        write(fd, result.string(), result.size());
    }

#undef CASE_APPEND_ENUM
    return NO_ERROR;
}

// IProCameraUser interface

void ProCamera2Client::disconnect() {
    ATRACE_CALL();
    Mutex::Autolock icl(mIProCameraUserLock);
    status_t res;

    // Allow both client and the media server to disconnect at all times
    int callingPid = getCallingPid();
    if (callingPid != mClientPid && callingPid != mServicePid) return;

    if (mDevice == 0) return;

    ALOGV("Camera %d: Shutting down", mCameraId);
    mFrameProcessor->removeListener(FRAME_PROCESSOR_LISTENER_MIN_ID,
                                    FRAME_PROCESSOR_LISTENER_MAX_ID,
                                    /*listener*/this);
    mFrameProcessor->requestExit();
    ALOGV("Camera %d: Waiting for threads", mCameraId);
    mFrameProcessor->join();
    ALOGV("Camera %d: Disconnecting device", mCameraId);

    mDevice->disconnect();

    mDevice.clear();

    ProClient::disconnect();
}

status_t ProCamera2Client::connect(const sp<IProCameraCallbacks>& client) {
    ATRACE_CALL();
    ALOGV("%s: E", __FUNCTION__);
    Mutex::Autolock icl(mIProCameraUserLock);

    if (mClientPid != 0 && getCallingPid() != mClientPid) {
        ALOGE("%s: Camera %d: Connection attempt from pid %d; "
                "current locked to pid %d", __FUNCTION__,
                mCameraId, getCallingPid(), mClientPid);
        return BAD_VALUE;
    }

    mClientPid = getCallingPid();

    mRemoteCallback = client;
    mSharedCameraCallbacks = client;

    return OK;
}

/** Device-related methods */

void ProCamera2Client::notifyError(int errorCode, int arg1, int arg2) {
    ALOGE("Error condition %d reported by HAL, arguments %d, %d", errorCode,
                                                                    arg1, arg2);
}

void ProCamera2Client::notifyShutter(int frameNumber, nsecs_t timestamp) {
    ALOGV("%s: Shutter notification for frame %d at time %lld", __FUNCTION__,
            frameNumber, timestamp);
}

void ProCamera2Client::notifyAutoFocus(uint8_t newState, int triggerId) {
    ALOGV("%s: Autofocus state now %d, last trigger %d",
            __FUNCTION__, newState, triggerId);

    SharedCameraCallbacks::Lock l(mSharedCameraCallbacks);
    if (l.mRemoteCallback != 0) {
        l.mRemoteCallback->notifyCallback(CAMERA_MSG_FOCUS_MOVE,
                1, 0);
    }
    if (l.mRemoteCallback != 0) {
        l.mRemoteCallback->notifyCallback(CAMERA_MSG_FOCUS,
                1, 0);
    }
}

void ProCamera2Client::notifyAutoExposure(uint8_t newState, int triggerId) {
    ALOGV("%s: Autoexposure state now %d, last trigger %d",
            __FUNCTION__, newState, triggerId);
}

void ProCamera2Client::notifyAutoWhitebalance(uint8_t newState, int triggerId) {
    ALOGV("%s: Auto-whitebalance state now %d, last trigger %d",
            __FUNCTION__, newState, triggerId);
}

int ProCamera2Client::getCameraId() const {
    return mCameraId;
}

const sp<Camera2Device>& ProCamera2Client::getCameraDevice() {
    return mDevice;
}

const sp<CameraService>& ProCamera2Client::getCameraService() {
    return mCameraService;
}

ProCamera2Client::SharedCameraCallbacks::Lock::Lock(
                                                 SharedCameraCallbacks &client):
        mRemoteCallback(client.mRemoteCallback),
        mSharedClient(client) {
    mSharedClient.mRemoteCallbackLock.lock();
}

ProCamera2Client::SharedCameraCallbacks::Lock::~Lock() {
    mSharedClient.mRemoteCallbackLock.unlock();
}

ProCamera2Client::SharedCameraCallbacks::SharedCameraCallbacks
                                         (const sp<IProCameraCallbacks>&client):
        mRemoteCallback(client) {
}

ProCamera2Client::SharedCameraCallbacks&
                             ProCamera2Client::SharedCameraCallbacks::operator=(
        const sp<IProCameraCallbacks>&client) {
    Mutex::Autolock l(mRemoteCallbackLock);
    mRemoteCallback = client;
    return *this;
}

void ProCamera2Client::SharedCameraCallbacks::clear() {
    Mutex::Autolock l(mRemoteCallbackLock);
    mRemoteCallback.clear();
}

void ProCamera2Client::onFrameAvailable(int32_t frameId,
                                        const CameraMetadata& frame) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);

    Mutex::Autolock icl(mIProCameraUserLock);
    SharedCameraCallbacks::Lock l(mSharedCameraCallbacks);

    if (mRemoteCallback != NULL) {
        CameraMetadata tmp(frame);
        camera_metadata_t* meta = tmp.release();
        ALOGV("%s: meta = %p ", __FUNCTION__, meta);
        mRemoteCallback->onResultReceived(frameId, meta);
        tmp.acquire(meta);
    }

}

} // namespace android
