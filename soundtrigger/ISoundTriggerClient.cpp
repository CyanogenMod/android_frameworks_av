/*
**
** Copyright 2014, The Android Open Source Project
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
#include <soundtrigger/ISoundTriggerClient.h>

namespace android {

enum {
    ON_RECOGNITION_EVENT = IBinder::FIRST_CALL_TRANSACTION,
};

class BpSoundTriggerClient: public BpInterface<ISoundTriggerClient>
{

public:
    BpSoundTriggerClient(const sp<IBinder>& impl)
        : BpInterface<ISoundTriggerClient>(impl)
    {
    }

    virtual void onRecognitionEvent(const sp<IMemory>& eventMemory)
    {
        Parcel data, reply;
        data.writeInterfaceToken(ISoundTriggerClient::getInterfaceDescriptor());
        data.writeStrongBinder(eventMemory->asBinder());
        remote()->transact(ON_RECOGNITION_EVENT,
                           data,
                           &reply);
    }
};

IMPLEMENT_META_INTERFACE(SoundTriggerClient,
                         "android.hardware.ISoundTriggerClient");

// ----------------------------------------------------------------------

status_t BnSoundTriggerClient::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch(code) {
        case ON_RECOGNITION_EVENT: {
            CHECK_INTERFACE(ISoundTriggerClient, data, reply);
            sp<IMemory> eventMemory = interface_cast<IMemory>(
                data.readStrongBinder());
            onRecognitionEvent(eventMemory);
            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

}; // namespace android
