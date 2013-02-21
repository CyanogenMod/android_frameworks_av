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

#ifndef ANDROID_SERVERS_CAMERA_PROCAMERA2CLIENT_H
#define ANDROID_SERVERS_CAMERA_PROCAMERA2CLIENT_H

#include "Camera2Device.h"
#include "CameraService.h"

namespace android {

class IMemory;
/**
 * Implements the binder IProCameraUser API,
 * meant for HAL2-level private API access.
 */
class ProCamera2Client :
        public CameraService::ProClient,
        public Camera2Device::NotificationListener
{
public:
    /**
     * IProCameraUser interface (see IProCameraUser for details)
     */
    virtual status_t      connect(const sp<IProCameraCallbacks>& callbacks);
    virtual void          disconnect();

    virtual status_t      exclusiveTryLock();
    virtual status_t      exclusiveLock();
    virtual status_t      exclusiveUnlock();

    virtual bool          hasExclusiveLock();

    // Note that the callee gets a copy of the metadata.
    virtual int           submitRequest(camera_metadata_t* metadata,
                                        bool streaming = false);
    virtual status_t      cancelRequest(int requestId);

    virtual status_t      requestStream(int streamId);
    virtual status_t      cancelStream(int streamId);

    virtual status_t      createStream(int width, int height, int format,
                                      const sp<IGraphicBufferProducer>& bufferProducer,
                                      /*out*/
                                      int* streamId);

    // Create a request object from a template.
    // -- Caller owns the newly allocated metadata
    virtual status_t      createDefaultRequest(int templateId,
                                               /*out*/
                                               camera_metadata** request);


    /**
     * Interface used by CameraService
     */

    ProCamera2Client(const sp<CameraService>& cameraService,
            const sp<IProCameraCallbacks>& remoteCallback,
            int cameraId,
            int cameraFacing,
            int clientPid,
            int servicePid);
    virtual ~ProCamera2Client();

    status_t initialize(camera_module_t *module);

    virtual status_t dump(int fd, const Vector<String16>& args);

    /**
     * Interface used by Camera2Device
     */

    virtual void notifyError(int errorCode, int arg1, int arg2);
    virtual void notifyShutter(int frameNumber, nsecs_t timestamp);
    virtual void notifyAutoFocus(uint8_t newState, int triggerId);
    virtual void notifyAutoExposure(uint8_t newState, int triggerId);
    virtual void notifyAutoWhitebalance(uint8_t newState, int triggerId);


    int getCameraId() const;
    const sp<Camera2Device>& getCameraDevice();
    const sp<CameraService>& getCameraService();

    /**
     * Interface used by independent components of ProCamera2Client.
     */

    // Simple class to ensure that access to IProCameraCallbacks is serialized
    // by requiring mRemoteCallbackLock to be locked before access to
    // mCameraClient is possible.
    class SharedCameraCallbacks {
      public:
        class Lock {
          public:
            Lock(SharedCameraCallbacks &client);
            ~Lock();
            sp<IProCameraCallbacks> &mRemoteCallback;
          private:
            SharedCameraCallbacks &mSharedClient;
        };
        SharedCameraCallbacks(const sp<IProCameraCallbacks>& client);
        SharedCameraCallbacks& operator=(const sp<IProCameraCallbacks>& client);
        void clear();
      private:
        sp<IProCameraCallbacks> mRemoteCallback;
        mutable Mutex mRemoteCallbackLock;
    } mSharedCameraCallbacks;

private:
    /** IProCameraUser interface-related private members */

    // Mutex that must be locked by methods implementing the IProCameraUser
    // interface. Ensures serialization between incoming IProCameraUser calls.
    // All methods below that append 'L' to the name assume that
    // mIProCameraUserLock is locked when they're called
    mutable Mutex mIProCameraUserLock;

    // Used with stream IDs
    static const int NO_STREAM = -1;

    /* Preview/Recording related members */

    sp<IBinder> mPreviewSurface;

    /** Preview callback related members */
    /** Camera2Device instance wrapping HAL2 entry */

    sp<Camera2Device> mDevice;

    /** Utility members */

    // Verify that caller is the owner of the camera
    status_t checkPid(const char *checkLocation) const;

    // Whether or not we have an exclusive lock on the device
    // - if no we can't modify the request queue.
    // note that creating/deleting streams we own is still OK
    bool mExclusiveLock;
};

}; // namespace android

#endif
