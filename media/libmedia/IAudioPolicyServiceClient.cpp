/*
 * Copyright (C) 2009 The Android Open Source Project
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

#define LOG_TAG "IAudioPolicyServiceClient"
#include <utils/Log.h>

#include <stdint.h>
#include <sys/types.h>

#include <binder/Parcel.h>

#include <media/IAudioPolicyServiceClient.h>
#include <media/AudioSystem.h>

namespace android {

enum {
    PORT_LIST_UPDATE = IBinder::FIRST_CALL_TRANSACTION,
    PATCH_LIST_UPDATE
};

class BpAudioPolicyServiceClient : public BpInterface<IAudioPolicyServiceClient>
{
public:
    BpAudioPolicyServiceClient(const sp<IBinder>& impl)
        : BpInterface<IAudioPolicyServiceClient>(impl)
    {
    }

    void onAudioPortListUpdate()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyServiceClient::getInterfaceDescriptor());
        remote()->transact(PORT_LIST_UPDATE, data, &reply, IBinder::FLAG_ONEWAY);
    }

    void onAudioPatchListUpdate()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyServiceClient::getInterfaceDescriptor());
        remote()->transact(PATCH_LIST_UPDATE, data, &reply, IBinder::FLAG_ONEWAY);
    }
};

IMPLEMENT_META_INTERFACE(AudioPolicyServiceClient, "android.media.IAudioPolicyServiceClient");

// ----------------------------------------------------------------------

status_t BnAudioPolicyServiceClient::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch (code) {
    case PORT_LIST_UPDATE: {
            CHECK_INTERFACE(IAudioPolicyServiceClient, data, reply);
            onAudioPortListUpdate();
            return NO_ERROR;
        } break;
    case PATCH_LIST_UPDATE: {
            CHECK_INTERFACE(IAudioPolicyServiceClient, data, reply);
            onAudioPatchListUpdate();
            return NO_ERROR;
        } break;
    default:
        return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

}; // namespace android
