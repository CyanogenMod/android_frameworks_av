/*
 * Copyright (C) 2014 The CyanogenMod Project
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

#include <unistd.h>
#include <binder/IBinder.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <utils/String16.h>

#include "PackageManager.h"

namespace android {

// This need to be synced with IPackageManager.java
const uint32_t GET_NAME_FROM_UID_TRANSACTION = IBinder::FIRST_CALL_TRANSACTION + 25;

String16 getNameForUid(int uid)
{
    int fd = -1;

    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> pm = sm->getService(String16("package"));
    if (pm != NULL) {
        Parcel data, reply;
        data.writeInterfaceToken(String16("android.content.pm.IPackageManager"));
        data.writeInt32(uid);
        status_t ret = pm->transact(GET_NAME_FROM_UID_TRANSACTION, data, &reply);
        if (ret == NO_ERROR) {
            int32_t exceptionCode = reply.readExceptionCode();
            if (!exceptionCode) {
                return reply.readString16();
            } else {
                // An exception was thrown back; fall through to return failure
                ALOGD("getNameForUid(%d) caught exception %d\n", uid, exceptionCode);
            }
        }
    }
    return String16();
}

} /* namespace android */
