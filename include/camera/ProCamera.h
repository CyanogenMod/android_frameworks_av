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
#include <utils/KeyedVector.h>
#include <gui/IGraphicBufferProducer.h>
#include <system/camera.h>
#include <camera/IProCameraCallbacks.h>
#include <camera/IProCameraUser.h>
#include <camera/Camera.h>
#include <gui/CpuConsumer.h>

struct camera_metadata;

namespace android {

// All callbacks on this class are concurrent
// (they come from separate threads)
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

    // OnBufferReceived and OnRequestReceived can come in with any order,
    // use android.sensor.timestamp and LockedBuffer.timestamp to correlate them

    // A new frame buffer has been received for this stream.
    // -- This callback only fires for createStreamCpu streams
    // -- Use buf.timestamp to correlate with metadata's
    //    android.sensor.timestamp
    // -- The buffer must not be accessed after this function call completes
    virtual void onBufferReceived(int streamId,
                                  const CpuConsumer::LockedBuffer& buf) = 0;
    /**
      * A new metadata buffer has been received.
      * -- Ownership of request passes on to the callee, free with
      *    free_camera_metadata.
      */
    virtual void onResultReceived(int32_t frameId, camera_metadata* result) = 0;
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
// TODO: remove requestStream, its useless.

    /**
      * Delete a stream.
      * Lock free.
      * Errors: BAD_VALUE if unknown stream ID.
      *         PERMISSION_DENIED if the stream wasn't yours
      */
    status_t deleteStream(int streamId);

    /**
      * Create a new HW stream, whose sink will be the window.
      * Lock free. Service maintains counter of streams.
      * Errors: -EBUSY if too many streams created
      */
    status_t createStream(int width, int height, int format,
                          const sp<Surface>& surface,
                          /*out*/
                          int* streamId);

    /**
      * Create a new HW stream, whose sink will be the SurfaceTexture.
      * Lock free. Service maintains counter of streams.
      * Errors: -EBUSY if too many streams created
      */
    status_t createStream(int width, int height, int format,
                          const sp<IGraphicBufferProducer>& bufferProducer,
                          /*out*/
                          int* streamId);
    status_t createStreamCpu(int width, int height, int format,
                          int heapCount,
                          /*out*/
                          int* streamId);

    // Create a request object from a template.
    status_t createDefaultRequest(int templateId,
                                 /*out*/
                                  camera_metadata** request) const;

    // Get number of cameras
    static int getNumberOfCameras();

    // Get static camera metadata
    static camera_metadata* getCameraInfo(int cameraId);

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

    virtual void        onResultReceived(int32_t frameId,
                                         camera_metadata* result);

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

    class ProFrameListener : public CpuConsumer::FrameAvailableListener {
    public:
        ProFrameListener(wp<ProCamera> camera, int streamID) {
            mCamera = camera;
            mStreamId = streamID;
        }

    protected:
        virtual void onFrameAvailable() {
            sp<ProCamera> c = mCamera.promote();
            if (c.get() != NULL) {
                c->onFrameAvailable(mStreamId);
            }
        }

    private:
        wp<ProCamera> mCamera;
        int mStreamId;
    };
    friend class ProFrameListener;

    struct StreamInfo
    {
        StreamInfo(int streamId) {
            this->streamID = streamId;
            cpuStream = false;
        }

        StreamInfo() {
            streamID = -1;
            cpuStream = false;
        }

        int  streamID;
        bool cpuStream;
        sp<CpuConsumer> cpuConsumer;
        sp<ProFrameListener> frameAvailableListener;
        sp<Surface> stc;
    };

    KeyedVector<int, StreamInfo> mStreams;


    void onFrameAvailable(int streamId);

    StreamInfo& getStreamInfo(int streamId);


};

}; // namespace android

#endif
