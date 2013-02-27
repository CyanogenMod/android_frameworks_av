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

#ifndef ANDROID_HARDWARE_ICAMERASERVICE_LISTENER_H
#define ANDROID_HARDWARE_ICAMERASERVICE_LISTENER_H

#include <utils/RefBase.h>
#include <binder/IInterface.h>
#include <binder/Parcel.h>
#include <hardware/camera_common.h>

namespace android {

class ICameraServiceListener : public IInterface
{
public:

    enum Status {
        // Device physically unplugged
        STATUS_PRESENT          = CAMERA_DEVICE_STATUS_PRESENT,
        // Device physically re-plugged
        STATUS_NOT_PRESENT      = CAMERA_DEVICE_STATUS_NOT_PRESENT,

        // Camera can be used exclusively
        STATUS_AVAILABLE        = 0x80000000,
        // Camera is in use by another app and cannot be used exclusively
        STATUS_NOT_AVAILABLE,

        // Use to initialize variables only
        STATUS_UNKNOWN          = 0xFFFFFFFF,
    };

    DECLARE_META_INTERFACE(CameraServiceListener);

    virtual void onStatusChanged(Status status, int32_t cameraId) = 0;
};

// ----------------------------------------------------------------------------

class BnCameraServiceListener : public BnInterface<ICameraServiceListener>
{
public:
    virtual status_t    onTransact( uint32_t code,
                                    const Parcel& data,
                                    Parcel* reply,
                                    uint32_t flags = 0);
};

}; // namespace android

#endif
