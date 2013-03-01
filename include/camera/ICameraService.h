/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef ANDROID_HARDWARE_ICAMERASERVICE_H
#define ANDROID_HARDWARE_ICAMERASERVICE_H

#include <utils/RefBase.h>
#include <binder/IInterface.h>
#include <binder/Parcel.h>

namespace android {

class ICamera;
class ICameraClient;
class IProCameraUser;
class IProCameraCallbacks;
class ICameraServiceListener;

class ICameraService : public IInterface
{
public:
    enum {
        GET_NUMBER_OF_CAMERAS = IBinder::FIRST_CALL_TRANSACTION,
        GET_CAMERA_INFO,
        CONNECT,
        CONNECT_PRO,
        ADD_LISTENER,
        REMOVE_LISTENER,
    };

    enum {
        USE_CALLING_UID = -1
    };

public:
    DECLARE_META_INTERFACE(CameraService);

    virtual int32_t  getNumberOfCameras() = 0;
    virtual status_t getCameraInfo(int cameraId,
                                          struct CameraInfo* cameraInfo) = 0;

    // Returns 'OK' if operation succeeded
    // - Errors: ALREADY_EXISTS if the listener was already added
    virtual status_t addListener(const sp<ICameraServiceListener>& listener)
                                                                            = 0;
    // Returns 'OK' if operation succeeded
    // - Errors: BAD_VALUE if specified listener was not in the listener list
    virtual status_t removeListener(const sp<ICameraServiceListener>& listener)
                                                                            = 0;
    /**
     * clientPackageName and clientUid are used for permissions checking.  if
     * clientUid == USE_CALLING_UID, then the calling UID is used instead. Only
     * trusted callers can set a clientUid other than USE_CALLING_UID.
     */
    virtual sp<ICamera> connect(const sp<ICameraClient>& cameraClient,
            int cameraId,
            const String16& clientPackageName,
            int clientUid) = 0;

    virtual sp<IProCameraUser> connect(const sp<IProCameraCallbacks>& cameraCb,
            int cameraId,
            const String16& clientPackageName,
            int clientUid) = 0;
};

// ----------------------------------------------------------------------------

class BnCameraService: public BnInterface<ICameraService>
{
public:
    virtual status_t    onTransact( uint32_t code,
                                    const Parcel& data,
                                    Parcel* reply,
                                    uint32_t flags = 0);
};

}; // namespace android

#endif
