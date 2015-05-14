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

//#define LOG_NDEBUG 0
#define LOG_TAG "IMediaHTTPService"
#include <utils/Log.h>

#include <media/IMediaHTTPService.h>

#include <binder/Parcel.h>
#include <media/IMediaHTTPConnection.h>

namespace android {

enum {
    MAKE_HTTP = IBinder::FIRST_CALL_TRANSACTION,
};

struct BpMediaHTTPService : public BpInterface<IMediaHTTPService> {
    BpMediaHTTPService(const sp<IBinder> &impl)
        : BpInterface<IMediaHTTPService>(impl) {
    }

    virtual sp<IMediaHTTPConnection> makeHTTPConnection() {
        Parcel data, reply;
        data.writeInterfaceToken(
                IMediaHTTPService::getInterfaceDescriptor());

        remote()->transact(MAKE_HTTP, data, &reply);

        status_t err = reply.readInt32();

        if (err != OK) {
            return NULL;
        }

        return interface_cast<IMediaHTTPConnection>(reply.readStrongBinder());
    }
};

IMPLEMENT_META_INTERFACE(
        MediaHTTPService, "android.media.IMediaHTTPService");

}  // namespace android

