/*
**
** Copyright 2015, The Android Open Source Project
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

#define LOG_TAG "IMediaCodecService"
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <stdint.h>
#include <sys/types.h>
#include <binder/Parcel.h>
#include <media/IMediaCodecService.h>

namespace android {

enum {
    GET_OMX = IBinder::FIRST_CALL_TRANSACTION
};

class BpMediaCodecService : public BpInterface<IMediaCodecService>
{
public:
    BpMediaCodecService(const sp<IBinder>& impl)
        : BpInterface<IMediaCodecService>(impl)
    {
    }

    virtual sp<IOMX> getOMX() {
        Parcel data, reply;
        data.writeInterfaceToken(IMediaCodecService::getInterfaceDescriptor());
        remote()->transact(GET_OMX, data, &reply);
        return interface_cast<IOMX>(reply.readStrongBinder());
    }

};

IMPLEMENT_META_INTERFACE(MediaCodecService, "android.media.IMediaCodecService");

// ----------------------------------------------------------------------

status_t BnMediaCodecService::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch (code) {

        case GET_OMX: {
            CHECK_INTERFACE(IMediaCodecService, data, reply);
            sp<IOMX> omx = getOMX();
            reply->writeStrongBinder(IInterface::asBinder(omx));
            return NO_ERROR;
        }
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

} // namespace android
