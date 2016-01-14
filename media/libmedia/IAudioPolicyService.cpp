/*
**
** Copyright 2009, The Android Open Source Project
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

#define LOG_TAG "IAudioPolicyService"
#include <utils/Log.h>

#include <stdint.h>
#include <sys/types.h>

#include <binder/Parcel.h>

#include <media/AudioEffect.h>
#include <media/IAudioPolicyService.h>

#include <system/audio.h>

namespace android {

enum {
    SET_DEVICE_CONNECTION_STATE = IBinder::FIRST_CALL_TRANSACTION,
    GET_DEVICE_CONNECTION_STATE,
    SET_PHONE_STATE,
    SET_RINGER_MODE,    // reserved, no longer used
    SET_FORCE_USE,
    GET_FORCE_USE,
    GET_OUTPUT,
    START_OUTPUT,
    STOP_OUTPUT,
    RELEASE_OUTPUT,
    GET_INPUT_FOR_ATTR,
    START_INPUT,
    STOP_INPUT,
    RELEASE_INPUT,
    INIT_STREAM_VOLUME,
    SET_STREAM_VOLUME,
    GET_STREAM_VOLUME,
    GET_STRATEGY_FOR_STREAM,
    GET_OUTPUT_FOR_EFFECT,
    REGISTER_EFFECT,
    UNREGISTER_EFFECT,
    IS_STREAM_ACTIVE,
    IS_SOURCE_ACTIVE,
    GET_DEVICES_FOR_STREAM,
    QUERY_DEFAULT_PRE_PROCESSING,
    SET_EFFECT_ENABLED,
    IS_STREAM_ACTIVE_REMOTELY,
    IS_OFFLOAD_SUPPORTED,
    LIST_AUDIO_PORTS,
    GET_AUDIO_PORT,
    CREATE_AUDIO_PATCH,
    RELEASE_AUDIO_PATCH,
    LIST_AUDIO_PATCHES,
    SET_AUDIO_PORT_CONFIG,
    REGISTER_CLIENT,
    GET_OUTPUT_FOR_ATTR,
    ACQUIRE_SOUNDTRIGGER_SESSION,
    RELEASE_SOUNDTRIGGER_SESSION,
    GET_PHONE_STATE,
    REGISTER_POLICY_MIXES,
    START_AUDIO_SOURCE,
    STOP_AUDIO_SOURCE,
    SET_AUDIO_PORT_CALLBACK_ENABLED,
    SET_EFFECT_SESSION_CALLBACK_ENABLED,
};

#define MAX_ITEMS_PER_LIST 1024

class BpAudioPolicyService : public BpInterface<IAudioPolicyService>
{
public:
    BpAudioPolicyService(const sp<IBinder>& impl)
        : BpInterface<IAudioPolicyService>(impl)
    {
    }

    virtual status_t setDeviceConnectionState(
                                    audio_devices_t device,
                                    audio_policy_dev_state_t state,
                                    const char *device_address,
                                    const char *device_name)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(static_cast <uint32_t>(device));
        data.writeInt32(static_cast <uint32_t>(state));
        data.writeCString(device_address);
        data.writeCString(device_name);
        remote()->transact(SET_DEVICE_CONNECTION_STATE, data, &reply);
        return static_cast <status_t> (reply.readInt32());
    }

    virtual audio_policy_dev_state_t getDeviceConnectionState(
                                    audio_devices_t device,
                                    const char *device_address)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(static_cast <uint32_t>(device));
        data.writeCString(device_address);
        remote()->transact(GET_DEVICE_CONNECTION_STATE, data, &reply);
        return static_cast <audio_policy_dev_state_t>(reply.readInt32());
    }

    virtual status_t setPhoneState(audio_mode_t state)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(state);
        remote()->transact(SET_PHONE_STATE, data, &reply);
        return static_cast <status_t> (reply.readInt32());
    }

    virtual status_t setForceUse(audio_policy_force_use_t usage, audio_policy_forced_cfg_t config)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(static_cast <uint32_t>(usage));
        data.writeInt32(static_cast <uint32_t>(config));
        remote()->transact(SET_FORCE_USE, data, &reply);
        return static_cast <status_t> (reply.readInt32());
    }

    virtual audio_policy_forced_cfg_t getForceUse(audio_policy_force_use_t usage)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(static_cast <uint32_t>(usage));
        remote()->transact(GET_FORCE_USE, data, &reply);
        return static_cast <audio_policy_forced_cfg_t> (reply.readInt32());
    }

    virtual audio_io_handle_t getOutput(
                                        audio_stream_type_t stream,
                                        uint32_t samplingRate,
                                        audio_format_t format,
                                        audio_channel_mask_t channelMask,
                                        audio_output_flags_t flags,
                                        const audio_offload_info_t *offloadInfo)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(static_cast <uint32_t>(stream));
        data.writeInt32(samplingRate);
        data.writeInt32(static_cast <uint32_t>(format));
        data.writeInt32(channelMask);
        data.writeInt32(static_cast <uint32_t>(flags));
        // hasOffloadInfo
        if (offloadInfo == NULL) {
            data.writeInt32(0);
        } else {
            data.writeInt32(1);
            data.write(offloadInfo, sizeof(audio_offload_info_t));
        }
        remote()->transact(GET_OUTPUT, data, &reply);
        return static_cast <audio_io_handle_t> (reply.readInt32());
    }

    virtual status_t getOutputForAttr(const audio_attributes_t *attr,
                                        audio_io_handle_t *output,
                                        audio_session_t session,
                                        audio_stream_type_t *stream,
                                        uid_t uid,
                                        uint32_t samplingRate,
                                        audio_format_t format,
                                        audio_channel_mask_t channelMask,
                                        audio_output_flags_t flags,
                                        audio_port_handle_t selectedDeviceId,
                                        const audio_offload_info_t *offloadInfo)
        {
            Parcel data, reply;
            data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
            if (attr == NULL) {
                if (stream == NULL) {
                    ALOGE("getOutputForAttr(): NULL audio attributes and stream type");
                    return BAD_VALUE;
                }
                if (*stream == AUDIO_STREAM_DEFAULT) {
                    ALOGE("getOutputForAttr unspecified stream type");
                    return BAD_VALUE;
                }
            }
            if (output == NULL) {
                ALOGE("getOutputForAttr NULL output - shouldn't happen");
                return BAD_VALUE;
            }
            if (attr == NULL) {
                data.writeInt32(0);
            } else {
                data.writeInt32(1);
                data.write(attr, sizeof(audio_attributes_t));
            }
            data.writeInt32(session);
            if (stream == NULL) {
                data.writeInt32(0);
            } else {
                data.writeInt32(1);
                data.writeInt32(*stream);
            }
            data.writeInt32(uid);
            data.writeInt32(samplingRate);
            data.writeInt32(static_cast <uint32_t>(format));
            data.writeInt32(channelMask);
            data.writeInt32(static_cast <uint32_t>(flags));
            data.writeInt32(selectedDeviceId);
            // hasOffloadInfo
            if (offloadInfo == NULL) {
                data.writeInt32(0);
            } else {
                data.writeInt32(1);
                data.write(offloadInfo, sizeof(audio_offload_info_t));
            }
            status_t status = remote()->transact(GET_OUTPUT_FOR_ATTR, data, &reply);
            if (status != NO_ERROR) {
                return status;
            }
            status = (status_t)reply.readInt32();
            if (status != NO_ERROR) {
                return status;
            }
            *output = (audio_io_handle_t)reply.readInt32();
            if (stream != NULL) {
                *stream = (audio_stream_type_t)reply.readInt32();
            }
            return status;
        }

    virtual status_t startOutput(audio_io_handle_t output,
                                 audio_stream_type_t stream,
                                 audio_session_t session)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(output);
        data.writeInt32((int32_t) stream);
        data.writeInt32((int32_t)session);
        remote()->transact(START_OUTPUT, data, &reply);
        return static_cast <status_t> (reply.readInt32());
    }

    virtual status_t stopOutput(audio_io_handle_t output,
                                audio_stream_type_t stream,
                                audio_session_t session)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(output);
        data.writeInt32((int32_t) stream);
        data.writeInt32((int32_t)session);
        remote()->transact(STOP_OUTPUT, data, &reply);
        return static_cast <status_t> (reply.readInt32());
    }

    virtual void releaseOutput(audio_io_handle_t output,
                               audio_stream_type_t stream,
                               audio_session_t session)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(output);
        data.writeInt32((int32_t)stream);
        data.writeInt32((int32_t)session);
        remote()->transact(RELEASE_OUTPUT, data, &reply);
    }

    virtual status_t getInputForAttr(const audio_attributes_t *attr,
                                     audio_io_handle_t *input,
                                     audio_session_t session,
                                     uid_t uid,
                                     uint32_t samplingRate,
                                     audio_format_t format,
                                     audio_channel_mask_t channelMask,
                                     audio_input_flags_t flags,
                                     audio_port_handle_t selectedDeviceId)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        if (attr == NULL) {
            ALOGE("getInputForAttr NULL attr - shouldn't happen");
            return BAD_VALUE;
        }
        if (input == NULL) {
            ALOGE("getInputForAttr NULL input - shouldn't happen");
            return BAD_VALUE;
        }
        data.write(attr, sizeof(audio_attributes_t));
        data.writeInt32(session);
        data.writeInt32(uid);
        data.writeInt32(samplingRate);
        data.writeInt32(static_cast <uint32_t>(format));
        data.writeInt32(channelMask);
        data.writeInt32(flags);
        data.writeInt32(selectedDeviceId);
        status_t status = remote()->transact(GET_INPUT_FOR_ATTR, data, &reply);
        if (status != NO_ERROR) {
            return status;
        }
        status = reply.readInt32();
        if (status != NO_ERROR) {
            return status;
        }
        *input = (audio_io_handle_t)reply.readInt32();
        return NO_ERROR;
    }

    virtual status_t startInput(audio_io_handle_t input,
                                audio_session_t session)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(input);
        data.writeInt32(session);
        remote()->transact(START_INPUT, data, &reply);
        return static_cast <status_t> (reply.readInt32());
    }

    virtual status_t stopInput(audio_io_handle_t input,
                               audio_session_t session)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(input);
        data.writeInt32(session);
        remote()->transact(STOP_INPUT, data, &reply);
        return static_cast <status_t> (reply.readInt32());
    }

    virtual void releaseInput(audio_io_handle_t input,
                              audio_session_t session)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(input);
        data.writeInt32(session);
        remote()->transact(RELEASE_INPUT, data, &reply);
    }

    virtual status_t initStreamVolume(audio_stream_type_t stream,
                                    int indexMin,
                                    int indexMax)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(static_cast <uint32_t>(stream));
        data.writeInt32(indexMin);
        data.writeInt32(indexMax);
        remote()->transact(INIT_STREAM_VOLUME, data, &reply);
        return static_cast <status_t> (reply.readInt32());
    }

    virtual status_t setStreamVolumeIndex(audio_stream_type_t stream,
                                          int index,
                                          audio_devices_t device)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(static_cast <uint32_t>(stream));
        data.writeInt32(index);
        data.writeInt32(static_cast <uint32_t>(device));
        remote()->transact(SET_STREAM_VOLUME, data, &reply);
        return static_cast <status_t> (reply.readInt32());
    }

    virtual status_t getStreamVolumeIndex(audio_stream_type_t stream,
                                          int *index,
                                          audio_devices_t device)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(static_cast <uint32_t>(stream));
        data.writeInt32(static_cast <uint32_t>(device));

        remote()->transact(GET_STREAM_VOLUME, data, &reply);
        int lIndex = reply.readInt32();
        if (index) *index = lIndex;
        return static_cast <status_t> (reply.readInt32());
    }

    virtual uint32_t getStrategyForStream(audio_stream_type_t stream)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(static_cast <uint32_t>(stream));
        remote()->transact(GET_STRATEGY_FOR_STREAM, data, &reply);
        return reply.readInt32();
    }

    virtual audio_devices_t getDevicesForStream(audio_stream_type_t stream)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(static_cast <uint32_t>(stream));
        remote()->transact(GET_DEVICES_FOR_STREAM, data, &reply);
        return (audio_devices_t) reply.readInt32();
    }

    virtual audio_io_handle_t getOutputForEffect(const effect_descriptor_t *desc)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.write(desc, sizeof(effect_descriptor_t));
        remote()->transact(GET_OUTPUT_FOR_EFFECT, data, &reply);
        return static_cast <audio_io_handle_t> (reply.readInt32());
    }

    virtual status_t registerEffect(const effect_descriptor_t *desc,
                                        audio_io_handle_t io,
                                        uint32_t strategy,
                                        int session,
                                        int id)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.write(desc, sizeof(effect_descriptor_t));
        data.writeInt32(io);
        data.writeInt32(strategy);
        data.writeInt32(session);
        data.writeInt32(id);
        remote()->transact(REGISTER_EFFECT, data, &reply);
        return static_cast <status_t> (reply.readInt32());
    }

    virtual status_t unregisterEffect(int id)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(id);
        remote()->transact(UNREGISTER_EFFECT, data, &reply);
        return static_cast <status_t> (reply.readInt32());
    }

    virtual status_t setEffectEnabled(int id, bool enabled)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(id);
        data.writeInt32(enabled);
        remote()->transact(SET_EFFECT_ENABLED, data, &reply);
        return static_cast <status_t> (reply.readInt32());
    }

    virtual bool isStreamActive(audio_stream_type_t stream, uint32_t inPastMs) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32((int32_t) stream);
        data.writeInt32(inPastMs);
        remote()->transact(IS_STREAM_ACTIVE, data, &reply);
        return reply.readInt32();
    }

    virtual bool isStreamActiveRemotely(audio_stream_type_t stream, uint32_t inPastMs) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32((int32_t) stream);
        data.writeInt32(inPastMs);
        remote()->transact(IS_STREAM_ACTIVE_REMOTELY, data, &reply);
        return reply.readInt32();
    }

    virtual bool isSourceActive(audio_source_t source) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32((int32_t) source);
        remote()->transact(IS_SOURCE_ACTIVE, data, &reply);
        return reply.readInt32();
    }

    virtual status_t queryDefaultPreProcessing(int audioSession,
                                               effect_descriptor_t *descriptors,
                                               uint32_t *count)
    {
        if (descriptors == NULL || count == NULL) {
            return BAD_VALUE;
        }
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(audioSession);
        data.writeInt32(*count);
        status_t status = remote()->transact(QUERY_DEFAULT_PRE_PROCESSING, data, &reply);
        if (status != NO_ERROR) {
            return status;
        }
        status = static_cast <status_t> (reply.readInt32());
        uint32_t retCount = reply.readInt32();
        if (retCount != 0) {
            uint32_t numDesc = (retCount < *count) ? retCount : *count;
            reply.read(descriptors, sizeof(effect_descriptor_t) * numDesc);
        }
        *count = retCount;
        return status;
    }

    virtual bool isOffloadSupported(const audio_offload_info_t& info)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.write(&info, sizeof(audio_offload_info_t));
        remote()->transact(IS_OFFLOAD_SUPPORTED, data, &reply);
        return reply.readInt32();
    }

    virtual status_t listAudioPorts(audio_port_role_t role,
                                    audio_port_type_t type,
                                    unsigned int *num_ports,
                                    struct audio_port *ports,
                                    unsigned int *generation)
    {
        if (num_ports == NULL || (*num_ports != 0 && ports == NULL) ||
                generation == NULL) {
            return BAD_VALUE;
        }
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        unsigned int numPortsReq = (ports == NULL) ? 0 : *num_ports;
        data.writeInt32(role);
        data.writeInt32(type);
        data.writeInt32(numPortsReq);
        status_t status = remote()->transact(LIST_AUDIO_PORTS, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
            *num_ports = (unsigned int)reply.readInt32();
        }
        if (status == NO_ERROR) {
            if (numPortsReq > *num_ports) {
                numPortsReq = *num_ports;
            }
            if (numPortsReq > 0) {
                reply.read(ports, numPortsReq * sizeof(struct audio_port));
            }
            *generation = reply.readInt32();
        }
        return status;
    }

    virtual status_t getAudioPort(struct audio_port *port)
    {
        if (port == NULL) {
            return BAD_VALUE;
        }
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.write(port, sizeof(struct audio_port));
        status_t status = remote()->transact(GET_AUDIO_PORT, data, &reply);
        if (status != NO_ERROR ||
                (status = (status_t)reply.readInt32()) != NO_ERROR) {
            return status;
        }
        reply.read(port, sizeof(struct audio_port));
        return status;
    }

    virtual status_t createAudioPatch(const struct audio_patch *patch,
                                       audio_patch_handle_t *handle)
    {
        if (patch == NULL || handle == NULL) {
            return BAD_VALUE;
        }
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.write(patch, sizeof(struct audio_patch));
        data.write(handle, sizeof(audio_patch_handle_t));
        status_t status = remote()->transact(CREATE_AUDIO_PATCH, data, &reply);
        if (status != NO_ERROR ||
                (status = (status_t)reply.readInt32()) != NO_ERROR) {
            return status;
        }
        reply.read(handle, sizeof(audio_patch_handle_t));
        return status;
    }

    virtual status_t releaseAudioPatch(audio_patch_handle_t handle)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.write(&handle, sizeof(audio_patch_handle_t));
        status_t status = remote()->transact(RELEASE_AUDIO_PATCH, data, &reply);
        if (status != NO_ERROR) {
            status = (status_t)reply.readInt32();
        }
        return status;
    }

    virtual status_t listAudioPatches(unsigned int *num_patches,
                                      struct audio_patch *patches,
                                      unsigned int *generation)
    {
        if (num_patches == NULL || (*num_patches != 0 && patches == NULL) ||
                generation == NULL) {
            return BAD_VALUE;
        }
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        unsigned int numPatchesReq = (patches == NULL) ? 0 : *num_patches;
        data.writeInt32(numPatchesReq);
        status_t status = remote()->transact(LIST_AUDIO_PATCHES, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
            *num_patches = (unsigned int)reply.readInt32();
        }
        if (status == NO_ERROR) {
            if (numPatchesReq > *num_patches) {
                numPatchesReq = *num_patches;
            }
            if (numPatchesReq > 0) {
                reply.read(patches, numPatchesReq * sizeof(struct audio_patch));
            }
            *generation = reply.readInt32();
        }
        return status;
    }

    virtual status_t setAudioPortConfig(const struct audio_port_config *config)
    {
        if (config == NULL) {
            return BAD_VALUE;
        }
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.write(config, sizeof(struct audio_port_config));
        status_t status = remote()->transact(SET_AUDIO_PORT_CONFIG, data, &reply);
        if (status != NO_ERROR) {
            status = (status_t)reply.readInt32();
        }
        return status;
    }

    virtual void registerClient(const sp<IAudioPolicyServiceClient>& client)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeStrongBinder(IInterface::asBinder(client));
        remote()->transact(REGISTER_CLIENT, data, &reply);
    }

    virtual void setAudioPortCallbacksEnabled(bool enabled)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(enabled ? 1 : 0);
        remote()->transact(SET_AUDIO_PORT_CALLBACK_ENABLED, data, &reply);
    }

    virtual status_t setEffectSessionCallbacksEnabled(bool enabled)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(enabled ? 1 : 0);
        status_t status = remote()->transact(SET_EFFECT_SESSION_CALLBACK_ENABLED, data, &reply);
        if (status != NO_ERROR) {
            return status;
        }
        return (status_t)reply.readInt32();
    }

    virtual status_t acquireSoundTriggerSession(audio_session_t *session,
                                            audio_io_handle_t *ioHandle,
                                            audio_devices_t *device)
    {
        if (session == NULL || ioHandle == NULL || device == NULL) {
            return BAD_VALUE;
        }
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        status_t status = remote()->transact(ACQUIRE_SOUNDTRIGGER_SESSION, data, &reply);
        if (status != NO_ERROR) {
            return status;
        }
        status = (status_t)reply.readInt32();
        if (status == NO_ERROR) {
            *session = (audio_session_t)reply.readInt32();
            *ioHandle = (audio_io_handle_t)reply.readInt32();
            *device = (audio_devices_t)reply.readInt32();
        }
        return status;
    }

    virtual status_t releaseSoundTriggerSession(audio_session_t session)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(session);
        status_t status = remote()->transact(RELEASE_SOUNDTRIGGER_SESSION, data, &reply);
        if (status != NO_ERROR) {
            return status;
        }
        return (status_t)reply.readInt32();
    }

    virtual audio_mode_t getPhoneState()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        status_t status = remote()->transact(GET_PHONE_STATE, data, &reply);
        if (status != NO_ERROR) {
            return AUDIO_MODE_INVALID;
        }
        return (audio_mode_t)reply.readInt32();
    }

    virtual status_t registerPolicyMixes(Vector<AudioMix> mixes, bool registration)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(registration ? 1 : 0);
        size_t size = mixes.size();
        if (size > MAX_MIXES_PER_POLICY) {
            size = MAX_MIXES_PER_POLICY;
        }
        size_t sizePosition = data.dataPosition();
        data.writeInt32(size);
        size_t finalSize = size;
        for (size_t i = 0; i < size; i++) {
            size_t position = data.dataPosition();
            if (mixes[i].writeToParcel(&data) != NO_ERROR) {
                data.setDataPosition(position);
                finalSize--;
            }
        }
        if (size != finalSize) {
            size_t position = data.dataPosition();
            data.setDataPosition(sizePosition);
            data.writeInt32(finalSize);
            data.setDataPosition(position);
        }
        status_t status = remote()->transact(REGISTER_POLICY_MIXES, data, &reply);
        if (status == NO_ERROR) {
            status = (status_t)reply.readInt32();
        }
        return status;
    }

    virtual status_t startAudioSource(const struct audio_port_config *source,
                                      const audio_attributes_t *attributes,
                                      audio_io_handle_t *handle)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        if (source == NULL || attributes == NULL || handle == NULL) {
            return BAD_VALUE;
        }
        data.write(source, sizeof(struct audio_port_config));
        data.write(attributes, sizeof(audio_attributes_t));
        status_t status = remote()->transact(START_AUDIO_SOURCE, data, &reply);
        if (status != NO_ERROR) {
            return status;
        }
        status = (status_t)reply.readInt32();
        if (status != NO_ERROR) {
            return status;
        }
        *handle = (audio_io_handle_t)reply.readInt32();
        return status;
    }

    virtual status_t stopAudioSource(audio_io_handle_t handle)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
        data.writeInt32(handle);
        status_t status = remote()->transact(STOP_AUDIO_SOURCE, data, &reply);
        if (status != NO_ERROR) {
            return status;
        }
        status = (status_t)reply.readInt32();
        return status;
    }
};

IMPLEMENT_META_INTERFACE(AudioPolicyService, "android.media.IAudioPolicyService");

// ----------------------------------------------------------------------


status_t BnAudioPolicyService::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch (code) {
        case SET_DEVICE_CONNECTION_STATE: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_devices_t device =
                    static_cast <audio_devices_t>(data.readInt32());
            audio_policy_dev_state_t state =
                    static_cast <audio_policy_dev_state_t>(data.readInt32());
            const char *device_address = data.readCString();
            const char *device_name = data.readCString();
            reply->writeInt32(static_cast<uint32_t> (setDeviceConnectionState(device,
                                                                              state,
                                                                              device_address,
                                                                              device_name)));
            return NO_ERROR;
        } break;

        case GET_DEVICE_CONNECTION_STATE: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_devices_t device =
                    static_cast<audio_devices_t> (data.readInt32());
            const char *device_address = data.readCString();
            reply->writeInt32(static_cast<uint32_t> (getDeviceConnectionState(device,
                                                                              device_address)));
            return NO_ERROR;
        } break;

        case SET_PHONE_STATE: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            reply->writeInt32(static_cast <uint32_t>(setPhoneState(
                    (audio_mode_t) data.readInt32())));
            return NO_ERROR;
        } break;

        case SET_FORCE_USE: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_policy_force_use_t usage = static_cast <audio_policy_force_use_t>(
                    data.readInt32());
            audio_policy_forced_cfg_t config =
                    static_cast <audio_policy_forced_cfg_t>(data.readInt32());
            reply->writeInt32(static_cast <uint32_t>(setForceUse(usage, config)));
            return NO_ERROR;
        } break;

        case GET_FORCE_USE: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_policy_force_use_t usage = static_cast <audio_policy_force_use_t>(
                    data.readInt32());
            reply->writeInt32(static_cast <uint32_t>(getForceUse(usage)));
            return NO_ERROR;
        } break;

        case GET_OUTPUT: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_stream_type_t stream =
                    static_cast <audio_stream_type_t>(data.readInt32());
            uint32_t samplingRate = data.readInt32();
            audio_format_t format = (audio_format_t) data.readInt32();
            audio_channel_mask_t channelMask = data.readInt32();
            audio_output_flags_t flags =
                    static_cast <audio_output_flags_t>(data.readInt32());
            bool hasOffloadInfo = data.readInt32() != 0;
            audio_offload_info_t offloadInfo;
            if (hasOffloadInfo) {
                data.read(&offloadInfo, sizeof(audio_offload_info_t));
            }
            audio_io_handle_t output = getOutput(stream,
                                                 samplingRate,
                                                 format,
                                                 channelMask,
                                                 flags,
                                                 hasOffloadInfo ? &offloadInfo : NULL);
            reply->writeInt32(static_cast <int>(output));
            return NO_ERROR;
        } break;

        case GET_OUTPUT_FOR_ATTR: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_attributes_t attr;
            bool hasAttributes = data.readInt32() != 0;
            if (hasAttributes) {
                data.read(&attr, sizeof(audio_attributes_t));
            }
            audio_session_t session = (audio_session_t)data.readInt32();
            audio_stream_type_t stream = AUDIO_STREAM_DEFAULT;
            bool hasStream = data.readInt32() != 0;
            if (hasStream) {
                stream = (audio_stream_type_t)data.readInt32();
            }
            uid_t uid = (uid_t)data.readInt32();
            uint32_t samplingRate = data.readInt32();
            audio_format_t format = (audio_format_t) data.readInt32();
            audio_channel_mask_t channelMask = data.readInt32();
            audio_output_flags_t flags =
                    static_cast <audio_output_flags_t>(data.readInt32());
            audio_port_handle_t selectedDeviceId = data.readInt32();
            bool hasOffloadInfo = data.readInt32() != 0;
            audio_offload_info_t offloadInfo;
            if (hasOffloadInfo) {
                data.read(&offloadInfo, sizeof(audio_offload_info_t));
            }
            audio_io_handle_t output = 0;
            status_t status = getOutputForAttr(hasAttributes ? &attr : NULL,
                    &output, session, &stream, uid,
                    samplingRate, format, channelMask,
                    flags, selectedDeviceId, hasOffloadInfo ? &offloadInfo : NULL);
            reply->writeInt32(status);
            reply->writeInt32(output);
            reply->writeInt32(stream);
            return NO_ERROR;
        } break;

        case START_OUTPUT: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_io_handle_t output = static_cast <audio_io_handle_t>(data.readInt32());
            audio_stream_type_t stream =
                                static_cast <audio_stream_type_t>(data.readInt32());
            audio_session_t session = (audio_session_t)data.readInt32();
            reply->writeInt32(static_cast <uint32_t>(startOutput(output,
                                                                 stream,
                                                                 session)));
            return NO_ERROR;
        } break;

        case STOP_OUTPUT: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_io_handle_t output = static_cast <audio_io_handle_t>(data.readInt32());
            audio_stream_type_t stream =
                                static_cast <audio_stream_type_t>(data.readInt32());
            audio_session_t session = (audio_session_t)data.readInt32();
            reply->writeInt32(static_cast <uint32_t>(stopOutput(output,
                                                                stream,
                                                                session)));
            return NO_ERROR;
        } break;

        case RELEASE_OUTPUT: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_io_handle_t output = static_cast <audio_io_handle_t>(data.readInt32());
            audio_stream_type_t stream = (audio_stream_type_t)data.readInt32();
            audio_session_t session = (audio_session_t)data.readInt32();
            releaseOutput(output, stream, session);
            return NO_ERROR;
        } break;

        case GET_INPUT_FOR_ATTR: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_attributes_t attr;
            data.read(&attr, sizeof(audio_attributes_t));
            audio_session_t session = (audio_session_t)data.readInt32();
            uid_t uid = (uid_t)data.readInt32();
            uint32_t samplingRate = data.readInt32();
            audio_format_t format = (audio_format_t) data.readInt32();
            audio_channel_mask_t channelMask = data.readInt32();
            audio_input_flags_t flags = (audio_input_flags_t) data.readInt32();
            audio_port_handle_t selectedDeviceId = (audio_port_handle_t) data.readInt32();
            audio_io_handle_t input = {};
            status_t status = getInputForAttr(&attr, &input, session, uid,
                                              samplingRate, format, channelMask,
                                              flags, selectedDeviceId);
            reply->writeInt32(status);
            if (status == NO_ERROR) {
                reply->writeInt32(input);
            }
            return NO_ERROR;
        } break;

        case START_INPUT: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_io_handle_t input = static_cast <audio_io_handle_t>(data.readInt32());
            audio_session_t session = static_cast <audio_session_t>(data.readInt32());
            reply->writeInt32(static_cast <uint32_t>(startInput(input, session)));
            return NO_ERROR;
        } break;

        case STOP_INPUT: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_io_handle_t input = static_cast <audio_io_handle_t>(data.readInt32());
            audio_session_t session = static_cast <audio_session_t>(data.readInt32());
            reply->writeInt32(static_cast <uint32_t>(stopInput(input, session)));
            return NO_ERROR;
        } break;

        case RELEASE_INPUT: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_io_handle_t input = static_cast <audio_io_handle_t>(data.readInt32());
            audio_session_t session = static_cast <audio_session_t>(data.readInt32());
            releaseInput(input, session);
            return NO_ERROR;
        } break;

        case INIT_STREAM_VOLUME: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_stream_type_t stream =
                    static_cast <audio_stream_type_t>(data.readInt32());
            int indexMin = data.readInt32();
            int indexMax = data.readInt32();
            reply->writeInt32(static_cast <uint32_t>(initStreamVolume(stream, indexMin,indexMax)));
            return NO_ERROR;
        } break;

        case SET_STREAM_VOLUME: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_stream_type_t stream =
                    static_cast <audio_stream_type_t>(data.readInt32());
            int index = data.readInt32();
            audio_devices_t device = static_cast <audio_devices_t>(data.readInt32());
            reply->writeInt32(static_cast <uint32_t>(setStreamVolumeIndex(stream,
                                                                          index,
                                                                          device)));
            return NO_ERROR;
        } break;

        case GET_STREAM_VOLUME: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_stream_type_t stream =
                    static_cast <audio_stream_type_t>(data.readInt32());
            audio_devices_t device = static_cast <audio_devices_t>(data.readInt32());
            int index = 0;
            status_t status = getStreamVolumeIndex(stream, &index, device);
            reply->writeInt32(index);
            reply->writeInt32(static_cast <uint32_t>(status));
            return NO_ERROR;
        } break;

        case GET_STRATEGY_FOR_STREAM: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_stream_type_t stream =
                    static_cast <audio_stream_type_t>(data.readInt32());
            reply->writeInt32(getStrategyForStream(stream));
            return NO_ERROR;
        } break;

        case GET_DEVICES_FOR_STREAM: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_stream_type_t stream =
                    static_cast <audio_stream_type_t>(data.readInt32());
            reply->writeInt32(static_cast <int>(getDevicesForStream(stream)));
            return NO_ERROR;
        } break;

        case GET_OUTPUT_FOR_EFFECT: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            effect_descriptor_t desc;
            data.read(&desc, sizeof(effect_descriptor_t));
            audio_io_handle_t output = getOutputForEffect(&desc);
            reply->writeInt32(static_cast <int>(output));
            return NO_ERROR;
        } break;

        case REGISTER_EFFECT: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            effect_descriptor_t desc;
            data.read(&desc, sizeof(effect_descriptor_t));
            audio_io_handle_t io = data.readInt32();
            uint32_t strategy = data.readInt32();
            int session = data.readInt32();
            int id = data.readInt32();
            reply->writeInt32(static_cast <int32_t>(registerEffect(&desc,
                                                                   io,
                                                                   strategy,
                                                                   session,
                                                                   id)));
            return NO_ERROR;
        } break;

        case UNREGISTER_EFFECT: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            int id = data.readInt32();
            reply->writeInt32(static_cast <int32_t>(unregisterEffect(id)));
            return NO_ERROR;
        } break;

        case SET_EFFECT_ENABLED: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            int id = data.readInt32();
            bool enabled = static_cast <bool>(data.readInt32());
            reply->writeInt32(static_cast <int32_t>(setEffectEnabled(id, enabled)));
            return NO_ERROR;
        } break;

        case IS_STREAM_ACTIVE: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_stream_type_t stream = (audio_stream_type_t) data.readInt32();
            uint32_t inPastMs = (uint32_t)data.readInt32();
            reply->writeInt32( isStreamActive(stream, inPastMs) );
            return NO_ERROR;
        } break;

        case IS_STREAM_ACTIVE_REMOTELY: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_stream_type_t stream = (audio_stream_type_t) data.readInt32();
            uint32_t inPastMs = (uint32_t)data.readInt32();
            reply->writeInt32( isStreamActiveRemotely(stream, inPastMs) );
            return NO_ERROR;
        } break;

        case IS_SOURCE_ACTIVE: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_source_t source = (audio_source_t) data.readInt32();
            reply->writeInt32( isSourceActive(source));
            return NO_ERROR;
        }

        case QUERY_DEFAULT_PRE_PROCESSING: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            int audioSession = data.readInt32();
            uint32_t count = data.readInt32();
            if (count > AudioEffect::kMaxPreProcessing) {
                count = AudioEffect::kMaxPreProcessing;
            }
            uint32_t retCount = count;
            effect_descriptor_t *descriptors = new effect_descriptor_t[count];
            status_t status = queryDefaultPreProcessing(audioSession, descriptors, &retCount);
            reply->writeInt32(status);
            if (status != NO_ERROR && status != NO_MEMORY) {
                retCount = 0;
            }
            reply->writeInt32(retCount);
            if (retCount != 0) {
                if (retCount < count) {
                    count = retCount;
                }
                reply->write(descriptors, sizeof(effect_descriptor_t) * count);
            }
            delete[] descriptors;
            return status;
        }

        case IS_OFFLOAD_SUPPORTED: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_offload_info_t info;
            data.read(&info, sizeof(audio_offload_info_t));
            bool isSupported = isOffloadSupported(info);
            reply->writeInt32(isSupported);
            return NO_ERROR;
        }

        case LIST_AUDIO_PORTS: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_port_role_t role = (audio_port_role_t)data.readInt32();
            audio_port_type_t type = (audio_port_type_t)data.readInt32();
            unsigned int numPortsReq = data.readInt32();
            if (numPortsReq > MAX_ITEMS_PER_LIST) {
                numPortsReq = MAX_ITEMS_PER_LIST;
            }
            unsigned int numPorts = numPortsReq;
            struct audio_port *ports =
                    (struct audio_port *)calloc(numPortsReq, sizeof(struct audio_port));
            if (ports == NULL) {
                reply->writeInt32(NO_MEMORY);
                reply->writeInt32(0);
                return NO_ERROR;
            }
            unsigned int generation;
            status_t status = listAudioPorts(role, type, &numPorts, ports, &generation);
            reply->writeInt32(status);
            reply->writeInt32(numPorts);

            if (status == NO_ERROR) {
                if (numPortsReq > numPorts) {
                    numPortsReq = numPorts;
                }
                reply->write(ports, numPortsReq * sizeof(struct audio_port));
                reply->writeInt32(generation);
            }
            free(ports);
            return NO_ERROR;
        }

        case GET_AUDIO_PORT: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            struct audio_port port = {};
            if (data.read(&port, sizeof(struct audio_port)) != NO_ERROR) {
                ALOGE("b/23912202");
            }
            status_t status = getAudioPort(&port);
            reply->writeInt32(status);
            if (status == NO_ERROR) {
                reply->write(&port, sizeof(struct audio_port));
            }
            return NO_ERROR;
        }

        case CREATE_AUDIO_PATCH: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            struct audio_patch patch;
            data.read(&patch, sizeof(struct audio_patch));
            audio_patch_handle_t handle = {};
            if (data.read(&handle, sizeof(audio_patch_handle_t)) != NO_ERROR) {
                ALOGE("b/23912202");
            }
            status_t status = createAudioPatch(&patch, &handle);
            reply->writeInt32(status);
            if (status == NO_ERROR) {
                reply->write(&handle, sizeof(audio_patch_handle_t));
            }
            return NO_ERROR;
        }

        case RELEASE_AUDIO_PATCH: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_patch_handle_t handle;
            data.read(&handle, sizeof(audio_patch_handle_t));
            status_t status = releaseAudioPatch(handle);
            reply->writeInt32(status);
            return NO_ERROR;
        }

        case LIST_AUDIO_PATCHES: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            unsigned int numPatchesReq = data.readInt32();
            if (numPatchesReq > MAX_ITEMS_PER_LIST) {
                numPatchesReq = MAX_ITEMS_PER_LIST;
            }
            unsigned int numPatches = numPatchesReq;
            struct audio_patch *patches =
                    (struct audio_patch *)calloc(numPatchesReq,
                                                 sizeof(struct audio_patch));
            if (patches == NULL) {
                reply->writeInt32(NO_MEMORY);
                reply->writeInt32(0);
                return NO_ERROR;
            }
            unsigned int generation;
            status_t status = listAudioPatches(&numPatches, patches, &generation);
            reply->writeInt32(status);
            reply->writeInt32(numPatches);
            if (status == NO_ERROR) {
                if (numPatchesReq > numPatches) {
                    numPatchesReq = numPatches;
                }
                reply->write(patches, numPatchesReq * sizeof(struct audio_patch));
                reply->writeInt32(generation);
            }
            free(patches);
            return NO_ERROR;
        }

        case SET_AUDIO_PORT_CONFIG: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            struct audio_port_config config;
            data.read(&config, sizeof(struct audio_port_config));
            status_t status = setAudioPortConfig(&config);
            reply->writeInt32(status);
            return NO_ERROR;
        }

        case REGISTER_CLIENT: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            sp<IAudioPolicyServiceClient> client = interface_cast<IAudioPolicyServiceClient>(
                    data.readStrongBinder());
            registerClient(client);
            return NO_ERROR;
        } break;

        case SET_AUDIO_PORT_CALLBACK_ENABLED: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            setAudioPortCallbacksEnabled(data.readInt32() == 1);
            return NO_ERROR;
        } break;

        case SET_EFFECT_SESSION_CALLBACK_ENABLED: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            status_t status = setEffectSessionCallbacksEnabled(data.readInt32() == 1);
            reply->writeInt32(status);
            return NO_ERROR;
        } break;

        case ACQUIRE_SOUNDTRIGGER_SESSION: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            sp<IAudioPolicyServiceClient> client = interface_cast<IAudioPolicyServiceClient>(
                    data.readStrongBinder());
            audio_session_t session = {};
            audio_io_handle_t ioHandle = {};
            audio_devices_t device = {};
            status_t status = acquireSoundTriggerSession(&session, &ioHandle, &device);
            reply->writeInt32(status);
            if (status == NO_ERROR) {
                reply->writeInt32(session);
                reply->writeInt32(ioHandle);
                reply->writeInt32(device);
            }
            return NO_ERROR;
        } break;

        case RELEASE_SOUNDTRIGGER_SESSION: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            sp<IAudioPolicyServiceClient> client = interface_cast<IAudioPolicyServiceClient>(
                    data.readStrongBinder());
            audio_session_t session = (audio_session_t)data.readInt32();
            status_t status = releaseSoundTriggerSession(session);
            reply->writeInt32(status);
            return NO_ERROR;
        } break;

        case GET_PHONE_STATE: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            reply->writeInt32((int32_t)getPhoneState());
            return NO_ERROR;
        } break;

        case REGISTER_POLICY_MIXES: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            bool registration = data.readInt32() == 1;
            Vector<AudioMix> mixes;
            size_t size = (size_t)data.readInt32();
            if (size > MAX_MIXES_PER_POLICY) {
                size = MAX_MIXES_PER_POLICY;
            }
            for (size_t i = 0; i < size; i++) {
                AudioMix mix;
                if (mix.readFromParcel((Parcel*)&data) == NO_ERROR) {
                    mixes.add(mix);
                }
            }
            status_t status = registerPolicyMixes(mixes, registration);
            reply->writeInt32(status);
            return NO_ERROR;
        } break;

        case START_AUDIO_SOURCE: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            struct audio_port_config source;
            data.read(&source, sizeof(struct audio_port_config));
            audio_attributes_t attributes;
            data.read(&attributes, sizeof(audio_attributes_t));
            audio_io_handle_t handle = {};
            status_t status = startAudioSource(&source, &attributes, &handle);
            reply->writeInt32(status);
            reply->writeInt32(handle);
            return NO_ERROR;
        } break;

        case STOP_AUDIO_SOURCE: {
            CHECK_INTERFACE(IAudioPolicyService, data, reply);
            audio_io_handle_t handle = (audio_io_handle_t)data.readInt32();
            status_t status = stopAudioSource(handle);
            reply->writeInt32(status);
            return NO_ERROR;
        } break;

        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

} // namespace android
