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

#define LOG_TAG "MediaCodecService"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include "MediaCodecService.h"

namespace android {

sp<IOMX> MediaCodecService::getOMX() {

    Mutex::Autolock autoLock(mLock);

    if (mOMX.get() == NULL) {
        mOMX = new OMX;
    }

    return mOMX;
}


status_t MediaCodecService::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
        uint32_t flags)
{
    return BnMediaCodecService::onTransact(code, data, reply, flags);
}

}   // namespace android
