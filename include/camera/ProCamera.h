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

#ifndef ANDROID_HARDWARE_PRO_CAMERA_H
#define ANDROID_HARDWARE_PRO_CAMERA_H

#include <utils/Timers.h>
#include <gui/IGraphicBufferProducer.h>
#include <system/camera.h>
#include <camera/IProCameraCallbacks.h>
#include <camera/IProCameraUser.h>
#include <camera/Camera.h>

struct camera_metadata;

namespace android {

// ref-counted object for callbacks
class ProCameraListener : public CameraListener
{
public:
    // Lock has been acquired. Write operations now available.
    virtual void onLockAcquired() = 0;
    // Lock has been released with exclusiveUnlock.
    virtual void onLockReleased() = 0;
    // Lock has been stolen by another client.
    virtual void onLockStolen() = 0;

    // Lock free.
    virtual void onTriggerNotify(int32_t msgType, int32_t ext1, int32_t ext2)
                                                                            = 0;
};

class ProCamera : public BnProCameraCallbacks, public IBinder::DeathRecipient
{
public:
    /**
     * Connect a shared camera. By default access is restricted to read only
     * (Lock free) operations. To be able to submit custom requests a lock needs
     * to be acquired with exclusive[Try]Lock.
     */
    static sp<ProCamera> connect(int cameraId);
    virtual void disconnect();
    virtual ~ProCamera();

    void setListener(const sp<ProCameraListener>& listener);

    /**
     * Exclusive Locks:
     * - We may request exclusive access to a camera if no other
     *   clients are using the camera. This works as a traditional
     *   client, writing/reading any camera state.
     * - An application opening the camera (a regular 'Camera') will
     *   always steal away the exclusive lock from a ProCamera,
     *   this will call onLockReleased.
     * - onLockAcquired will be called again once it is possible
     *   to again exclusively lock the camera.
     *
     */

    /**
     * All exclusiveLock/unlock functions are asynchronous. The remote endpoint
     * shall not block while waiting to acquire the lock. Instead the lock
     * notifications will come in asynchronously on the listener.
     */

    /**
      * Attempt to acquire the lock instantly (non-blocking)
      * - If this succeeds, you do not need to wait for onLockAcquired
      *   but the event will still be fired
      *
      * Returns -EBUSY if already locked. 0 on success.
      */
    status_t exclusiveTryLock();
    // always returns 0. wait for onLockAcquired before lock is acquired.
    status_t exclusiveLock();
    // release a lock if we have one, or cancel the lock request.
    status_t exclusiveUnlock();

    // exclusive lock = do whatever we want. no lock = read only.
    bool hasExclusiveLock();

    /**
     * < 0 error, >= 0 the request ID. streaming to have the request repeat
     *    until cancelled.
     * The request queue is flushed when a lock is released or stolen
     *    if not locked will return PERMISSION_DENIED
     */
    int submitRequest(const struct camera_metadata* metadata,
                                                        bool streaming = false);
    // if not locked will return PERMISSION_DENIED, BAD_VALUE if requestId bad
    status_t cancelRequest(int requestId);

    /**
     * Ask for a stream to be enabled.
     * Lock free. Service maintains counter of streams.
     */
    status_t requestStream(int streamId);
    /**
     * Ask for a stream to be disabled.
     * Lock free. Service maintains counter of streams.
     * Errors: BAD_VALUE if unknown stream ID.
     */
    status_t cancelStream(int streamId);

    sp<IProCameraUser>         remote();

protected:
    ////////////////////////////////////////////////////////
    // IProCameraCallbacks implementation
    ////////////////////////////////////////////////////////
    virtual void        notifyCallback(int32_t msgType, int32_t ext,
                                       int32_t ext2);
    virtual void        dataCallback(int32_t msgType,
                                     const sp<IMemory>& dataPtr,
                                     camera_frame_metadata_t *metadata);
    virtual void        dataCallbackTimestamp(nsecs_t timestamp,
                                              int32_t msgType,
                                              const sp<IMemory>& dataPtr);
    virtual void        onLockStatusChanged(
                                IProCameraCallbacks::LockStatus newLockStatus);

    class DeathNotifier: public IBinder::DeathRecipient
    {
    public:
        DeathNotifier() {
        }

        virtual void binderDied(const wp<IBinder>& who);
    };

private:
    ProCamera();

    virtual void binderDied(const wp<IBinder>& who);

    // helper function to obtain camera service handle
    static const sp<ICameraService>& getCameraService();

    static sp<DeathNotifier> mDeathNotifier;

    sp<IProCameraUser>  mCamera;
    status_t            mStatus;

    sp<ProCameraListener>  mListener;

    friend class DeathNotifier;

    static  Mutex               mLock;
    static  sp<ICameraService>  mCameraService;


};

}; // namespace android

#endif
