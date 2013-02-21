/*
**
** Copyright (C) 2013, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

//#define LOG_NDEBUG 0
#define LOG_TAG "ProCamera"
#include <utils/Log.h>
#include <utils/threads.h>
#include <utils/Mutex.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/IMemory.h>

#include <camera/ProCamera.h>
#include <camera/ICameraService.h>
#include <camera/IProCameraUser.h>
#include <camera/IProCameraCallbacks.h>

#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>

namespace android {

// client singleton for camera service binder interface
Mutex ProCamera::mLock;
sp<ICameraService> ProCamera::mCameraService;
sp<ProCamera::DeathNotifier> ProCamera::mDeathNotifier;

// establish binder interface to camera service
const sp<ICameraService>& ProCamera::getCameraService()
{
    Mutex::Autolock _l(mLock);
    if (mCameraService.get() == 0) {
        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder;
        do {
            binder = sm->getService(String16("media.camera"));
            if (binder != 0)
                break;
            ALOGW("CameraService not published, waiting...");
            usleep(500000); // 0.5 s
        } while(true);
        if (mDeathNotifier == NULL) {
            mDeathNotifier = new DeathNotifier();
        }
        binder->linkToDeath(mDeathNotifier);
        mCameraService = interface_cast<ICameraService>(binder);
    }
    ALOGE_IF(mCameraService==0, "no CameraService!?");
    return mCameraService;
}

sp<ProCamera> ProCamera::connect(int cameraId)
{
    ALOGV("connect");
    sp<ProCamera> c = new ProCamera();
    sp<IProCameraCallbacks> cl = c;
    const sp<ICameraService>& cs = getCameraService();
    if (cs != 0) {
        c->mCamera = cs->connect(cl, cameraId);
    }
    if (c->mCamera != 0) {
        c->mCamera->asBinder()->linkToDeath(c);
        c->mStatus = NO_ERROR;
    } else {
        c.clear();
    }
    return c;
}

void ProCamera::disconnect()
{
    ALOGV("disconnect");
    if (mCamera != 0) {
        mCamera->disconnect();
        mCamera->asBinder()->unlinkToDeath(this);
        mCamera = 0;
    }
}

ProCamera::ProCamera()
{
}

ProCamera::~ProCamera()
{

}

sp<IProCameraUser> ProCamera::remote()
{
    return mCamera;
}

void ProCamera::binderDied(const wp<IBinder>& who) {
    ALOGW("IProCameraUser died");
    notifyCallback(CAMERA_MSG_ERROR, CAMERA_ERROR_SERVER_DIED, 0);
}

void ProCamera::DeathNotifier::binderDied(const wp<IBinder>& who) {
    ALOGV("binderDied");
    Mutex::Autolock _l(ProCamera::mLock);
    ProCamera::mCameraService.clear();
    ALOGW("Camera service died!");
}

void ProCamera::setListener(const sp<ProCameraListener>& listener)
{
    Mutex::Autolock _l(mLock);
    mListener = listener;
}


// callback from camera service
void ProCamera::notifyCallback(int32_t msgType, int32_t ext1, int32_t ext2)
{
    sp<ProCameraListener> listener;
    {
        Mutex::Autolock _l(mLock);
        listener = mListener;
    }
    if (listener != NULL) {
        listener->notify(msgType, ext1, ext2);
    }
}

// callback from camera service when frame or image is ready
void ProCamera::dataCallback(int32_t msgType, const sp<IMemory>& dataPtr,
                          camera_frame_metadata_t *metadata)
{
    sp<ProCameraListener> listener;
    {
        Mutex::Autolock _l(mLock);
        listener = mListener;
    }
    if (listener != NULL) {
        listener->postData(msgType, dataPtr, metadata);
    }
}

// callback from camera service when timestamped frame is ready
void ProCamera::dataCallbackTimestamp(nsecs_t timestamp, int32_t msgType,
                                                    const sp<IMemory>& dataPtr)
{
    sp<ProCameraListener> listener;
    {
        Mutex::Autolock _l(mLock);
        listener = mListener;
    }
    if (listener != NULL) {
        listener->postDataTimestamp(timestamp, msgType, dataPtr);
    } else {
        ALOGW("No listener was set. Drop a recording frame.");
    }
}

/* IProCameraUser's implementation */

void ProCamera::onLockStatusChanged(
                                 IProCameraCallbacks::LockStatus newLockStatus)
{
    ALOGV("%s: newLockStatus = %d", __FUNCTION__, newLockStatus);

    sp<ProCameraListener> listener;
    {
        Mutex::Autolock _l(mLock);
        listener = mListener;
    }
    if (listener != NULL) {
        switch (newLockStatus) {
            case IProCameraCallbacks::LOCK_ACQUIRED:
                listener->onLockAcquired();
                break;
            case IProCameraCallbacks::LOCK_RELEASED:
                listener->onLockReleased();
                break;
            case IProCameraCallbacks::LOCK_STOLEN:
                listener->onLockStolen();
                break;
            default:
                ALOGE("%s: Unknown lock status: %d",
                      __FUNCTION__, newLockStatus);
        }
    }
}

status_t ProCamera::exclusiveTryLock()
{
    sp <IProCameraUser> c = mCamera;
    if (c == 0) return NO_INIT;

    return c->exclusiveTryLock();
}
status_t ProCamera::exclusiveLock()
{
    sp <IProCameraUser> c = mCamera;
    if (c == 0) return NO_INIT;

    return c->exclusiveLock();
}
status_t ProCamera::exclusiveUnlock()
{
    sp <IProCameraUser> c = mCamera;
    if (c == 0) return NO_INIT;

    return c->exclusiveUnlock();
}
bool ProCamera::hasExclusiveLock()
{
    sp <IProCameraUser> c = mCamera;
    if (c == 0) return NO_INIT;

    return c->hasExclusiveLock();
}

// Note that the callee gets a copy of the metadata.
int ProCamera::submitRequest(const struct camera_metadata* metadata,
                             bool streaming)
{
    sp <IProCameraUser> c = mCamera;
    if (c == 0) return NO_INIT;

    return c->submitRequest(const_cast<struct camera_metadata*>(metadata),
                            streaming);
}

status_t ProCamera::cancelRequest(int requestId)
{
    sp <IProCameraUser> c = mCamera;
    if (c == 0) return NO_INIT;

    return c->cancelRequest(requestId);
}

status_t ProCamera::requestStream(int streamId)
{
    sp <IProCameraUser> c = mCamera;
    if (c == 0) return NO_INIT;

    return c->requestStream(streamId);
}
status_t ProCamera::cancelStream(int streamId)
{
    sp <IProCameraUser> c = mCamera;
    if (c == 0) return NO_INIT;

    return c->cancelStream(streamId);
}

}; // namespace android
