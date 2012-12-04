/*
** Copyright (c) 2012, The Linux Foundation. All rights reserved.
** Not a Contribution, Apache license notifications and license are retained
** for attribution purposes only.
**
** Copyright 2007, The Android Open Source Project
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

#include <media/IDirectTrackClient.h>

namespace android {

enum {
    NOTIFY = IBinder::FIRST_CALL_TRANSACTION,
};

class BpDirectTrackClient: public BpInterface<IDirectTrackClient>
{
public:
    BpDirectTrackClient(const sp<IBinder>& impl)
        : BpInterface<IDirectTrackClient>(impl)
    {
    }

    virtual void notify(int msg)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IDirectTrackClient::getInterfaceDescriptor());
        data.writeInt32(msg);
        remote()->transact(NOTIFY, data, &reply, IBinder::FLAG_ONEWAY);
    }
};

IMPLEMENT_META_INTERFACE(DirectTrackClient, "android.media.IDirectTrackClient");

// ----------------------------------------------------------------------

status_t BnDirectTrackClient::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch (code) {
        case NOTIFY: {
            CHECK_INTERFACE(IDirectTrackClient, data, reply);
            int msg = data.readInt32();
            notify(msg);
            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

}; // namespace android
