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

#define LOG_TAG "BpCameraServiceProxy"

#include <stdint.h>

#include <binder/Parcel.h>

#include <camera/ICameraServiceProxy.h>

namespace android {

class BpCameraServiceProxy: public BpInterface<ICameraServiceProxy> {
public:
    BpCameraServiceProxy(const sp<IBinder>& impl) : BpInterface<ICameraServiceProxy>(impl) {}

    virtual void pingForUserUpdate() {
        Parcel data, reply;
        data.writeInterfaceToken(ICameraServiceProxy::getInterfaceDescriptor());
        remote()->transact(BnCameraServiceProxy::PING_FOR_USER_UPDATE, data, &reply,
                IBinder::FLAG_ONEWAY);
    }
};


IMPLEMENT_META_INTERFACE(CameraServiceProxy, "android.hardware.ICameraServiceProxy");

status_t BnCameraServiceProxy::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
        uint32_t flags) {
    switch(code) {
        case PING_FOR_USER_UPDATE: {
            CHECK_INTERFACE(ICameraServiceProxy, data, reply);
            pingForUserUpdate();
            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}
}; // namespace android

