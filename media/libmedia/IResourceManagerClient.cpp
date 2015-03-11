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

#include <utils/RefBase.h>
#include <binder/IInterface.h>
#include <binder/Parcel.h>

#include <media/IResourceManagerClient.h>

namespace android {

enum {
    RECLAIM_RESOURCE = IBinder::FIRST_CALL_TRANSACTION,
};

class BpResourceManagerClient: public BpInterface<IResourceManagerClient>
{
public:
    BpResourceManagerClient(const sp<IBinder> &impl)
        : BpInterface<IResourceManagerClient>(impl)
    {
    }

    virtual bool reclaimResource() {
        Parcel data, reply;
        data.writeInterfaceToken(IResourceManagerClient::getInterfaceDescriptor());

        bool ret = false;
        status_t status = remote()->transact(RECLAIM_RESOURCE, data, &reply);
        if (status == NO_ERROR) {
            ret = (bool)reply.readInt32();
        }
        return ret;
    }
};

IMPLEMENT_META_INTERFACE(ResourceManagerClient, "android.media.IResourceManagerClient");

// ----------------------------------------------------------------------

status_t BnResourceManagerClient::onTransact(
    uint32_t code, const Parcel &data, Parcel *reply, uint32_t flags)
{
    switch (code) {
        case RECLAIM_RESOURCE: {
            CHECK_INTERFACE(IResourceManagerClient, data, reply);
            bool ret = reclaimResource();
            reply->writeInt32(ret);
            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

}; // namespace android
