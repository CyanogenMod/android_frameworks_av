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

#include <stdint.h>
#include <sys/types.h>
#include <binder/IMemory.h>
#include <binder/Parcel.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <radio/IRadioClient.h>

namespace android {

enum {
    ON_EVENT = IBinder::FIRST_CALL_TRANSACTION,
};

class BpRadioClient: public BpInterface<IRadioClient>
{

public:
    BpRadioClient(const sp<IBinder>& impl)
        : BpInterface<IRadioClient>(impl)
    {
    }

    virtual void onEvent(const sp<IMemory>& eventMemory)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IRadioClient::getInterfaceDescriptor());
        data.writeStrongBinder(IInterface::asBinder(eventMemory));
        remote()->transact(ON_EVENT,
                           data,
                           &reply);
    }
};

IMPLEMENT_META_INTERFACE(RadioClient,
                         "android.hardware.IRadioClient");

// ----------------------------------------------------------------------

status_t BnRadioClient::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch(code) {
        case ON_EVENT: {
            CHECK_INTERFACE(IRadioClient, data, reply);
            sp<IMemory> eventMemory = interface_cast<IMemory>(
                data.readStrongBinder());
            onEvent(eventMemory);
            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }   return NO_ERROR;
}

// ----------------------------------------------------------------------------

}; // namespace android
