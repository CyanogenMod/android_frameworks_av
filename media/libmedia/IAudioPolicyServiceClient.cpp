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
    PATCH_LIST_UPDATE,
    MIX_STATE_UPDATE,
    RECORDING_CONFIGURATION_UPDATE
};

// ----------------------------------------------------------------------
inline void readAudioConfigBaseFromParcel(const Parcel& data, audio_config_base_t *config) {
    config->sample_rate = data.readUint32();
    config->channel_mask = (audio_channel_mask_t) data.readInt32();
    config->format = (audio_format_t) data.readInt32();
}

inline void writeAudioConfigBaseToParcel(Parcel& data, const audio_config_base_t *config)
{
    data.writeUint32(config->sample_rate);
    data.writeInt32((int32_t) config->channel_mask);
    data.writeInt32((int32_t) config->format);
}

// ----------------------------------------------------------------------
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

    void onDynamicPolicyMixStateUpdate(String8 regId, int32_t state)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyServiceClient::getInterfaceDescriptor());
        data.writeString8(regId);
        data.writeInt32(state);
        remote()->transact(MIX_STATE_UPDATE, data, &reply, IBinder::FLAG_ONEWAY);
    }

    void onRecordingConfigurationUpdate(int event, audio_session_t session,
            audio_source_t source, const audio_config_base_t *clientConfig,
            const audio_config_base_t *deviceConfig) {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyServiceClient::getInterfaceDescriptor());
        data.writeInt32(event);
        data.writeInt32(session);
        data.writeInt32(source);
        writeAudioConfigBaseToParcel(data, clientConfig);
        writeAudioConfigBaseToParcel(data, deviceConfig);
        remote()->transact(RECORDING_CONFIGURATION_UPDATE, data, &reply, IBinder::FLAG_ONEWAY);
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
    case MIX_STATE_UPDATE: {
            CHECK_INTERFACE(IAudioPolicyServiceClient, data, reply);
            String8 regId = data.readString8();
            int32_t state = data.readInt32();
            onDynamicPolicyMixStateUpdate(regId, state);
            return NO_ERROR;
        } break;
    case RECORDING_CONFIGURATION_UPDATE: {
            CHECK_INTERFACE(IAudioPolicyServiceClient, data, reply);
            int event = (int) data.readInt32();
            audio_session_t session = (audio_session_t) data.readInt32();
            audio_source_t source = (audio_source_t) data.readInt32();
            audio_config_base_t clientConfig;
            audio_config_base_t deviceConfig;
            readAudioConfigBaseFromParcel(data, &clientConfig);
            readAudioConfigBaseFromParcel(data, &deviceConfig);
            onRecordingConfigurationUpdate(event, session, source, &clientConfig, &deviceConfig);
            return NO_ERROR;
        } break;
    default:
        return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

} // namespace android
