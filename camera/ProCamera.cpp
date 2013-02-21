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

#include <system/camera_metadata.h>

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

void ProCamera::onResultReceived(int32_t frameId, camera_metadata* result) {
    ALOGV("%s: frameId = %d, result = %p", __FUNCTION__, frameId, result);

    sp<ProCameraListener> listener;
    {
        Mutex::Autolock _l(mLock);
        listener = mListener;
    }
    if (listener != NULL) {
        listener->onResultReceived(frameId, result);
    } else {
        free_camera_metadata(result);
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

status_t ProCamera::deleteStream(int streamId)
{
    sp <IProCameraUser> c = mCamera;
    if (c == 0) return NO_INIT;

    status_t s = c->cancelStream(streamId);

    mStreams.removeItem(streamId);

    return s;
}

status_t ProCamera::createStream(int width, int height, int format,
                          const sp<Surface>& surface,
                          /*out*/
                          int* streamId)
{
    *streamId = -1;

    ALOGV("%s: createStreamW %dx%d (fmt=0x%x)", __FUNCTION__, width, height,
                                                                       format);

    if (surface == 0) {
        return BAD_VALUE;
    }

    return createStream(width, height, format, surface->getIGraphicBufferProducer(),
                        streamId);
}

status_t ProCamera::createStream(int width, int height, int format,
                          const sp<IGraphicBufferProducer>& bufferProducer,
                          /*out*/
                          int* streamId) {
    *streamId = -1;

    ALOGV("%s: createStreamT %dx%d (fmt=0x%x)", __FUNCTION__, width, height,
                                                                       format);

    if (bufferProducer == 0) {
        return BAD_VALUE;
    }

    sp <IProCameraUser> c = mCamera;
    status_t stat = c->createStream(width, height, format, bufferProducer,
                                    streamId);

    if (stat == OK) {
        StreamInfo s(*streamId);

        mStreams.add(*streamId, s);
    }

    return stat;
}

status_t ProCamera::createStreamCpu(int width, int height, int format,
                          int heapCount,
                          /*out*/
                          int* streamId)
{
    ALOGV("%s: createStreamW %dx%d (fmt=0x%x)", __FUNCTION__, width, height,
                                                                        format);

    sp <IProCameraUser> c = mCamera;
    if (c == 0) return NO_INIT;

    sp<CpuConsumer> cc = new CpuConsumer(heapCount);
    cc->setName(String8("ProCamera::mCpuConsumer"));

    sp<Surface> stc = new Surface(
        cc->getProducerInterface());

    status_t s = createStream(width, height, format, stc->getIGraphicBufferProducer(),
                        streamId);

    if (s != OK) {
        ALOGE("%s: Failure to create stream %dx%d (fmt=0x%x)", __FUNCTION__,
                    width, height, format);
        return s;
    }

    sp<ProFrameListener> frameAvailableListener =
        new ProFrameListener(this, *streamId);

    getStreamInfo(*streamId).cpuStream = true;
    getStreamInfo(*streamId).cpuConsumer = cc;
    getStreamInfo(*streamId).stc = stc;
    // for lifetime management
    getStreamInfo(*streamId).frameAvailableListener = frameAvailableListener;

    cc->setFrameAvailableListener(frameAvailableListener);

    return s;
}

int ProCamera::getNumberOfCameras() {
    const sp<ICameraService> cs = getCameraService();

    if (!cs.get()) {
        return DEAD_OBJECT;
    }
    return cs->getNumberOfCameras();
}

camera_metadata* ProCamera::getCameraInfo(int cameraId) {
    ALOGV("%s: cameraId = %d", __FUNCTION__, cameraId);

    sp <IProCameraUser> c = mCamera;
    if (c == 0) return NULL;

    camera_metadata* ptr = NULL;
    status_t status = c->getCameraInfo(cameraId, &ptr);

    if (status != OK) {
        ALOGE("%s: Failed to get camera info, error = %d", __FUNCTION__, status);
    }

    return ptr;
}

status_t ProCamera::createDefaultRequest(int templateId,
                                             camera_metadata** request) const {
    ALOGV("%s: templateId = %d", __FUNCTION__, templateId);

    sp <IProCameraUser> c = mCamera;
    if (c == 0) return NO_INIT;

    return c->createDefaultRequest(templateId, request);
}

void ProCamera::onFrameAvailable(int streamId) {
    ALOGV("%s: streamId = %d", __FUNCTION__, streamId);

    sp<ProCameraListener> listener = mListener;
    if (listener.get() != NULL) {
        StreamInfo& stream = getStreamInfo(streamId);

        CpuConsumer::LockedBuffer buf;

        status_t stat = stream.cpuConsumer->lockNextBuffer(&buf);
        if (stat != OK) {
            ALOGE("%s: Failed to lock buffer, error code = %d", __FUNCTION__,
                   stat);
            return;
        }

        listener->onBufferReceived(streamId, buf);
        stat = stream.cpuConsumer->unlockBuffer(buf);

        if (stat != OK) {
            ALOGE("%s: Failed to unlock buffer, error code = %d", __FUNCTION__,
                   stat);
        }
    }
}

ProCamera::StreamInfo& ProCamera::getStreamInfo(int streamId) {
    return mStreams.editValueFor(streamId);
}

}; // namespace android
