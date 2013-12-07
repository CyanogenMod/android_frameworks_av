/*
** Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
** Not a Contribution.
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

#define LOG_TAG "IAudioFlinger"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <stdint.h>
#include <sys/types.h>

#include <binder/Parcel.h>

#include <media/IAudioFlinger.h>

namespace android {

enum {
    CREATE_TRACK = IBinder::FIRST_CALL_TRANSACTION,
    OPEN_RECORD,
    SAMPLE_RATE,
    RESERVED,   // obsolete, was CHANNEL_COUNT
    FORMAT,
    FRAME_COUNT,
    LATENCY,
    SET_MASTER_VOLUME,
    SET_MASTER_MUTE,
    MASTER_VOLUME,
    MASTER_MUTE,
    SET_STREAM_VOLUME,
    SET_STREAM_MUTE,
    STREAM_VOLUME,
    STREAM_MUTE,
    SET_MODE,
    SET_MIC_MUTE,
    GET_MIC_MUTE,
    SET_PARAMETERS,
    GET_PARAMETERS,
    REGISTER_CLIENT,
    GET_INPUTBUFFERSIZE,
    OPEN_OUTPUT,
    OPEN_DUPLICATE_OUTPUT,
    CLOSE_OUTPUT,
    SUSPEND_OUTPUT,
    RESTORE_OUTPUT,
    OPEN_INPUT,
    CLOSE_INPUT,
    SET_STREAM_OUTPUT,
    SET_VOICE_VOLUME,
    GET_RENDER_POSITION,
    GET_INPUT_FRAMES_LOST,
    NEW_AUDIO_SESSION_ID,
    ACQUIRE_AUDIO_SESSION_ID,
    RELEASE_AUDIO_SESSION_ID,
    QUERY_NUM_EFFECTS,
    QUERY_EFFECT,
    GET_EFFECT_DESCRIPTOR,
    CREATE_EFFECT,
    MOVE_EFFECTS,
    LOAD_HW_MODULE,
    GET_PRIMARY_OUTPUT_SAMPLING_RATE,
    GET_PRIMARY_OUTPUT_FRAME_COUNT,
    SET_LOW_RAM_DEVICE,
#ifdef QCOM_HARDWARE
    CREATE_DIRECT_TRACK,
#endif
};

class BpAudioFlinger : public BpInterface<IAudioFlinger>
{
public:
    BpAudioFlinger(const sp<IBinder>& impl)
        : BpInterface<IAudioFlinger>(impl)
    {
    }

    virtual sp<IAudioTrack> createTrack(
                                audio_stream_type_t streamType,
                                uint32_t sampleRate,
                                audio_format_t format,
                                audio_channel_mask_t channelMask,
                                size_t frameCount,
                                track_flags_t *flags,
                                const sp<IMemory>& sharedBuffer,
                                audio_io_handle_t output,
                                pid_t tid,
                                int *sessionId,
                                String8& name,
                                int clientUid,
                                status_t *status)
    {
        Parcel data, reply;
        sp<IAudioTrack> track;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) streamType);
        data.writeInt32(sampleRate);
        data.writeInt32(format);
        data.writeInt32(channelMask);
        data.writeInt32(frameCount);
        track_flags_t lFlags = flags != NULL ? *flags : (track_flags_t) TRACK_DEFAULT;
        data.writeInt32(lFlags);
        if (sharedBuffer != 0) {
            data.writeInt32(true);
            data.writeStrongBinder(sharedBuffer->asBinder());
        } else {
            data.writeInt32(false);
        }
        data.writeInt32((int32_t) output);
        data.writeInt32((int32_t) tid);
        int lSessionId = 0;
        if (sessionId != NULL) {
            lSessionId = *sessionId;
        }
        data.writeInt32(lSessionId);
        data.writeInt32(clientUid);
        status_t lStatus = remote()->transact(CREATE_TRACK, data, &reply);
        if (lStatus != NO_ERROR) {
            ALOGE("createTrack error: %s", strerror(-lStatus));
        } else {
            lFlags = reply.readInt32();
            if (flags != NULL) {
                *flags = lFlags;
            }
            lSessionId = reply.readInt32();
            if (sessionId != NULL) {
                *sessionId = lSessionId;
            }
            name = reply.readString8();
            lStatus = reply.readInt32();
            track = interface_cast<IAudioTrack>(reply.readStrongBinder());
        }
        if (status) {
            *status = lStatus;
        }
        return track;
    }

#ifdef QCOM_HARDWARE
    virtual sp<IDirectTrack> createDirectTrack(
                                pid_t pid,
                                uint32_t sampleRate,
                                audio_channel_mask_t channelMask,
                                audio_io_handle_t output,
                                int *sessionId,
                                IDirectTrackClient* client,
                                audio_stream_type_t streamType,
                                status_t *status)
    {
        Parcel data, reply;
        sp<IDirectTrack> track;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32(pid);
        data.writeInt32(sampleRate);
        data.writeInt32(channelMask);
        data.writeInt32((int32_t)output);
        int lSessionId = 0;
        if (sessionId != NULL) {
            lSessionId = *sessionId;
        }
        data.writeInt32(lSessionId);
        data.write(client, sizeof(IDirectTrackClient));
        data.writeInt32((int32_t) streamType);
        status_t lStatus = remote()->transact(CREATE_DIRECT_TRACK, data, &reply);
        if (lStatus != NO_ERROR) {
            ALOGE("createDirectTrack error: %s", strerror(-lStatus));
        } else {
            lSessionId = reply.readInt32();
            if (sessionId != NULL) {
                *sessionId = lSessionId;
            }
            lStatus = reply.readInt32();
            track = interface_cast<IDirectTrack>(reply.readStrongBinder());
        }
        if (status) {
            *status = lStatus;
        }
        return track;
    }
#endif

    virtual sp<IAudioRecord> openRecord(
                                audio_io_handle_t input,
                                uint32_t sampleRate,
                                audio_format_t format,
                                audio_channel_mask_t channelMask,
                                size_t frameCount,
                                track_flags_t *flags,
                                pid_t tid,
                                int *sessionId,
                                status_t *status)
    {
        Parcel data, reply;
        sp<IAudioRecord> record;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) input);
        data.writeInt32(sampleRate);
        data.writeInt32(format);
        data.writeInt32(channelMask);
        data.writeInt32(frameCount);
        track_flags_t lFlags = flags != NULL ? *flags : (track_flags_t) TRACK_DEFAULT;
        data.writeInt32(lFlags);
        data.writeInt32((int32_t) tid);
        int lSessionId = 0;
        if (sessionId != NULL) {
            lSessionId = *sessionId;
        }
        data.writeInt32(lSessionId);
        status_t lStatus = remote()->transact(OPEN_RECORD, data, &reply);
        if (lStatus != NO_ERROR) {
            ALOGE("openRecord error: %s", strerror(-lStatus));
        } else {
            lFlags = reply.readInt32();
            if (flags != NULL) {
                *flags = lFlags;
            }
            lSessionId = reply.readInt32();
            if (sessionId != NULL) {
                *sessionId = lSessionId;
            }
            lStatus = reply.readInt32();
            record = interface_cast<IAudioRecord>(reply.readStrongBinder());
            if (lStatus == NO_ERROR) {
                if (record == 0) {
                    ALOGE("openRecord should have returned an IAudioRecord");
                    lStatus = UNKNOWN_ERROR;
                }
            } else {
                if (record != 0) {
                    ALOGE("openRecord returned an IAudioRecord but with status %d", lStatus);
                    record.clear();
                }
            }
        }
        if (status) {
            *status = lStatus;
        }
        return record;
    }

    virtual uint32_t sampleRate(audio_io_handle_t output) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) output);
        remote()->transact(SAMPLE_RATE, data, &reply);
        return reply.readInt32();
    }

    virtual audio_format_t format(audio_io_handle_t output) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) output);
        remote()->transact(FORMAT, data, &reply);
        return (audio_format_t) reply.readInt32();
    }

    virtual size_t frameCount(audio_io_handle_t output) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) output);
        remote()->transact(FRAME_COUNT, data, &reply);
        return reply.readInt32();
    }

    virtual uint32_t latency(audio_io_handle_t output) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) output);
        remote()->transact(LATENCY, data, &reply);
        return reply.readInt32();
    }

    virtual status_t setMasterVolume(float value)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeFloat(value);
        remote()->transact(SET_MASTER_VOLUME, data, &reply);
        return reply.readInt32();
    }

    virtual status_t setMasterMute(bool muted)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32(muted);
        remote()->transact(SET_MASTER_MUTE, data, &reply);
        return reply.readInt32();
    }

    virtual float masterVolume() const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        remote()->transact(MASTER_VOLUME, data, &reply);
        return reply.readFloat();
    }

    virtual bool masterMute() const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        remote()->transact(MASTER_MUTE, data, &reply);
        return reply.readInt32();
    }

    virtual status_t setStreamVolume(audio_stream_type_t stream, float value,
            audio_io_handle_t output)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) stream);
        data.writeFloat(value);
        data.writeInt32((int32_t) output);
        remote()->transact(SET_STREAM_VOLUME, data, &reply);
        return reply.readInt32();
    }

    virtual status_t setStreamMute(audio_stream_type_t stream, bool muted)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) stream);
        data.writeInt32(muted);
        remote()->transact(SET_STREAM_MUTE, data, &reply);
        return reply.readInt32();
    }

    virtual float streamVolume(audio_stream_type_t stream, audio_io_handle_t output) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) stream);
        data.writeInt32((int32_t) output);
        remote()->transact(STREAM_VOLUME, data, &reply);
        return reply.readFloat();
    }

    virtual bool streamMute(audio_stream_type_t stream) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) stream);
        remote()->transact(STREAM_MUTE, data, &reply);
        return reply.readInt32();
    }

    virtual status_t setMode(audio_mode_t mode)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32(mode);
        remote()->transact(SET_MODE, data, &reply);
        return reply.readInt32();
    }

    virtual status_t setMicMute(bool state)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32(state);
        remote()->transact(SET_MIC_MUTE, data, &reply);
        return reply.readInt32();
    }

    virtual bool getMicMute() const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        remote()->transact(GET_MIC_MUTE, data, &reply);
        return reply.readInt32();
    }

    virtual status_t setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) ioHandle);
        data.writeString8(keyValuePairs);
        remote()->transact(SET_PARAMETERS, data, &reply);
        return reply.readInt32();
    }

    virtual String8 getParameters(audio_io_handle_t ioHandle, const String8& keys) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) ioHandle);
        data.writeString8(keys);
        remote()->transact(GET_PARAMETERS, data, &reply);
        return reply.readString8();
    }

    virtual void registerClient(const sp<IAudioFlingerClient>& client)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeStrongBinder(client->asBinder());
        remote()->transact(REGISTER_CLIENT, data, &reply);
    }

    virtual size_t getInputBufferSize(uint32_t sampleRate, audio_format_t format,
            audio_channel_mask_t channelMask) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32(sampleRate);
        data.writeInt32(format);
        data.writeInt32(channelMask);
        remote()->transact(GET_INPUTBUFFERSIZE, data, &reply);
        return reply.readInt32();
    }

    virtual audio_io_handle_t openOutput(audio_module_handle_t module,
                                         audio_devices_t *pDevices,
                                         uint32_t *pSamplingRate,
                                         audio_format_t *pFormat,
                                         audio_channel_mask_t *pChannelMask,
                                         uint32_t *pLatencyMs,
                                         audio_output_flags_t flags,
                                         const audio_offload_info_t *offloadInfo)
    {
        Parcel data, reply;
        audio_devices_t devices = pDevices != NULL ? *pDevices : (audio_devices_t)0;
        uint32_t samplingRate = pSamplingRate != NULL ? *pSamplingRate : 0;
        audio_format_t format = pFormat != NULL ? *pFormat : AUDIO_FORMAT_DEFAULT;
        audio_channel_mask_t channelMask = pChannelMask != NULL ?
                *pChannelMask : (audio_channel_mask_t)0;
        uint32_t latency = pLatencyMs != NULL ? *pLatencyMs : 0;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32(module);
        data.writeInt32(devices);
        data.writeInt32(samplingRate);
        data.writeInt32(format);
        data.writeInt32(channelMask);
        data.writeInt32(latency);
        data.writeInt32((int32_t) flags);
        if (offloadInfo == NULL) {
            data.writeInt32(0);
        } else {
            data.writeInt32(1);
            data.write(offloadInfo, sizeof(audio_offload_info_t));
        }
        remote()->transact(OPEN_OUTPUT, data, &reply);
        audio_io_handle_t output = (audio_io_handle_t) reply.readInt32();
        ALOGV("openOutput() returned output, %d", output);
        devices = (audio_devices_t)reply.readInt32();
        if (pDevices != NULL) *pDevices = devices;
        samplingRate = reply.readInt32();
        if (pSamplingRate != NULL) *pSamplingRate = samplingRate;
        format = (audio_format_t) reply.readInt32();
        if (pFormat != NULL) *pFormat = format;
        channelMask = (audio_channel_mask_t)reply.readInt32();
        if (pChannelMask != NULL) *pChannelMask = channelMask;
        latency = reply.readInt32();
        if (pLatencyMs != NULL) *pLatencyMs = latency;
        return output;
    }

    virtual audio_io_handle_t openDuplicateOutput(audio_io_handle_t output1,
            audio_io_handle_t output2)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) output1);
        data.writeInt32((int32_t) output2);
        remote()->transact(OPEN_DUPLICATE_OUTPUT, data, &reply);
        return (audio_io_handle_t) reply.readInt32();
    }

    virtual status_t closeOutput(audio_io_handle_t output)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) output);
        remote()->transact(CLOSE_OUTPUT, data, &reply);
        return reply.readInt32();
    }

    virtual status_t suspendOutput(audio_io_handle_t output)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) output);
        remote()->transact(SUSPEND_OUTPUT, data, &reply);
        return reply.readInt32();
    }

    virtual status_t restoreOutput(audio_io_handle_t output)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) output);
        remote()->transact(RESTORE_OUTPUT, data, &reply);
        return reply.readInt32();
    }

    virtual audio_io_handle_t openInput(audio_module_handle_t module,
                                        audio_devices_t *pDevices,
                                        uint32_t *pSamplingRate,
                                        audio_format_t *pFormat,
                                        audio_channel_mask_t *pChannelMask)
    {
        Parcel data, reply;
        audio_devices_t devices = pDevices != NULL ? *pDevices : (audio_devices_t)0;
        uint32_t samplingRate = pSamplingRate != NULL ? *pSamplingRate : 0;
        audio_format_t format = pFormat != NULL ? *pFormat : AUDIO_FORMAT_DEFAULT;
        audio_channel_mask_t channelMask = pChannelMask != NULL ?
                *pChannelMask : (audio_channel_mask_t)0;

        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32(module);
        data.writeInt32(devices);
        data.writeInt32(samplingRate);
        data.writeInt32(format);
        data.writeInt32(channelMask);
        remote()->transact(OPEN_INPUT, data, &reply);
        audio_io_handle_t input = (audio_io_handle_t) reply.readInt32();
        devices = (audio_devices_t)reply.readInt32();
        if (pDevices != NULL) *pDevices = devices;
        samplingRate = reply.readInt32();
        if (pSamplingRate != NULL) *pSamplingRate = samplingRate;
        format = (audio_format_t) reply.readInt32();
        if (pFormat != NULL) *pFormat = format;
        channelMask = (audio_channel_mask_t)reply.readInt32();
        if (pChannelMask != NULL) *pChannelMask = channelMask;
        return input;
    }

    virtual status_t closeInput(int input)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32(input);
        remote()->transact(CLOSE_INPUT, data, &reply);
        return reply.readInt32();
    }

    virtual status_t setStreamOutput(audio_stream_type_t stream, audio_io_handle_t output)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) stream);
        data.writeInt32((int32_t) output);
        remote()->transact(SET_STREAM_OUTPUT, data, &reply);
        return reply.readInt32();
    }

    virtual status_t setVoiceVolume(float volume)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeFloat(volume);
        remote()->transact(SET_VOICE_VOLUME, data, &reply);
        return reply.readInt32();
    }

    virtual status_t getRenderPosition(size_t *halFrames, size_t *dspFrames,
            audio_io_handle_t output) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) output);
        remote()->transact(GET_RENDER_POSITION, data, &reply);
        status_t status = reply.readInt32();
        if (status == NO_ERROR) {
            uint32_t tmp = reply.readInt32();
            if (halFrames) {
                *halFrames = tmp;
            }
            tmp = reply.readInt32();
            if (dspFrames) {
                *dspFrames = tmp;
            }
        }
        return status;
    }

    virtual size_t getInputFramesLost(audio_io_handle_t ioHandle) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int32_t) ioHandle);
        remote()->transact(GET_INPUT_FRAMES_LOST, data, &reply);
        return reply.readInt32();
    }

    virtual int newAudioSessionId()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        status_t status = remote()->transact(NEW_AUDIO_SESSION_ID, data, &reply);
        int id = 0;
        if (status == NO_ERROR) {
            id = reply.readInt32();
        }
        return id;
    }

    virtual void acquireAudioSessionId(int audioSession)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32(audioSession);
        remote()->transact(ACQUIRE_AUDIO_SESSION_ID, data, &reply);
    }

    virtual void releaseAudioSessionId(int audioSession)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32(audioSession);
        remote()->transact(RELEASE_AUDIO_SESSION_ID, data, &reply);
    }

    virtual status_t queryNumberEffects(uint32_t *numEffects) const
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        status_t status = remote()->transact(QUERY_NUM_EFFECTS, data, &reply);
        if (status != NO_ERROR) {
            return status;
        }
        status = reply.readInt32();
        if (status != NO_ERROR) {
            return status;
        }
        if (numEffects != NULL) {
            *numEffects = (uint32_t)reply.readInt32();
        }
        return NO_ERROR;
    }

    virtual status_t queryEffect(uint32_t index, effect_descriptor_t *pDescriptor) const
    {
        if (pDescriptor == NULL) {
            return BAD_VALUE;
        }
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32(index);
        status_t status = remote()->transact(QUERY_EFFECT, data, &reply);
        if (status != NO_ERROR) {
            return status;
        }
        status = reply.readInt32();
        if (status != NO_ERROR) {
            return status;
        }
        reply.read(pDescriptor, sizeof(effect_descriptor_t));
        return NO_ERROR;
    }

    virtual status_t getEffectDescriptor(const effect_uuid_t *pUuid,
            effect_descriptor_t *pDescriptor) const
    {
        if (pUuid == NULL || pDescriptor == NULL) {
            return BAD_VALUE;
        }
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.write(pUuid, sizeof(effect_uuid_t));
        status_t status = remote()->transact(GET_EFFECT_DESCRIPTOR, data, &reply);
        if (status != NO_ERROR) {
            return status;
        }
        status = reply.readInt32();
        if (status != NO_ERROR) {
            return status;
        }
        reply.read(pDescriptor, sizeof(effect_descriptor_t));
        return NO_ERROR;
    }

    virtual sp<IEffect> createEffect(
                                    effect_descriptor_t *pDesc,
                                    const sp<IEffectClient>& client,
                                    int32_t priority,
                                    audio_io_handle_t output,
                                    int sessionId,
                                    status_t *status,
                                    int *id,
                                    int *enabled)
    {
        Parcel data, reply;
        sp<IEffect> effect;

        if (pDesc == NULL) {
            return effect;
            if (status) {
                *status = BAD_VALUE;
            }
        }

        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.write(pDesc, sizeof(effect_descriptor_t));
        data.writeStrongBinder(client->asBinder());
        data.writeInt32(priority);
        data.writeInt32((int32_t) output);
        data.writeInt32(sessionId);

        status_t lStatus = remote()->transact(CREATE_EFFECT, data, &reply);
        if (lStatus != NO_ERROR) {
            ALOGE("createEffect error: %s", strerror(-lStatus));
        } else {
            lStatus = reply.readInt32();
            int tmp = reply.readInt32();
            if (id) {
                *id = tmp;
            }
            tmp = reply.readInt32();
            if (enabled != NULL) {
                *enabled = tmp;
            }
            effect = interface_cast<IEffect>(reply.readStrongBinder());
            reply.read(pDesc, sizeof(effect_descriptor_t));
        }
        if (status) {
            *status = lStatus;
        }

        return effect;
    }

    virtual status_t moveEffects(int session, audio_io_handle_t srcOutput,
            audio_io_handle_t dstOutput)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32(session);
        data.writeInt32((int32_t) srcOutput);
        data.writeInt32((int32_t) dstOutput);
        remote()->transact(MOVE_EFFECTS, data, &reply);
        return reply.readInt32();
    }

    virtual audio_module_handle_t loadHwModule(const char *name)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeCString(name);
        remote()->transact(LOAD_HW_MODULE, data, &reply);
        return (audio_module_handle_t) reply.readInt32();
    }

    virtual uint32_t getPrimaryOutputSamplingRate()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        remote()->transact(GET_PRIMARY_OUTPUT_SAMPLING_RATE, data, &reply);
        return reply.readInt32();
    }

    virtual size_t getPrimaryOutputFrameCount()
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        remote()->transact(GET_PRIMARY_OUTPUT_FRAME_COUNT, data, &reply);
        return reply.readInt32();
    }

    virtual status_t setLowRamDevice(bool isLowRamDevice)
    {
        Parcel data, reply;
        data.writeInterfaceToken(IAudioFlinger::getInterfaceDescriptor());
        data.writeInt32((int) isLowRamDevice);
        remote()->transact(SET_LOW_RAM_DEVICE, data, &reply);
        return reply.readInt32();
    }

};

IMPLEMENT_META_INTERFACE(AudioFlinger, "android.media.IAudioFlinger");

// ----------------------------------------------------------------------

status_t BnAudioFlinger::onTransact(
    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    switch (code) {
        case CREATE_TRACK: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            int streamType = data.readInt32();
            uint32_t sampleRate = data.readInt32();
            audio_format_t format = (audio_format_t) data.readInt32();
            audio_channel_mask_t channelMask = data.readInt32();
            size_t frameCount = data.readInt32();
            track_flags_t flags = (track_flags_t) data.readInt32();
            bool haveSharedBuffer = data.readInt32() != 0;
            sp<IMemory> buffer;
            if (haveSharedBuffer) {
                buffer = interface_cast<IMemory>(data.readStrongBinder());
            }
            audio_io_handle_t output = (audio_io_handle_t) data.readInt32();
            pid_t tid = (pid_t) data.readInt32();
            int sessionId = data.readInt32();
            int clientUid = data.readInt32();
            String8 name;
            status_t status;
            sp<IAudioTrack> track;
            if ((haveSharedBuffer && (buffer == 0)) ||
                    ((buffer != 0) && (buffer->pointer() == NULL))) {
                ALOGW("CREATE_TRACK: cannot retrieve shared memory");
                status = DEAD_OBJECT;
            } else {
                track = createTrack(
                        (audio_stream_type_t) streamType, sampleRate, format,
                        channelMask, frameCount, &flags, buffer, output, tid,
                        &sessionId, name, clientUid, &status);
            }
            reply->writeInt32(flags);
            reply->writeInt32(sessionId);
            reply->writeString8(name);
            reply->writeInt32(status);
            reply->writeStrongBinder(track->asBinder());
            return NO_ERROR;
        } break;
#ifdef QCOM_HARDWARE
        case CREATE_DIRECT_TRACK: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            pid_t pid = data.readInt32();
            uint32_t sampleRate = data.readInt32();
            audio_channel_mask_t channelMask = data.readInt32();
            audio_io_handle_t output = (audio_io_handle_t) data.readInt32();
            int sessionId = data.readInt32();
            IDirectTrackClient* client;
            data.read(client,sizeof(IDirectTrackClient));
            int streamType = data.readInt32();
            status_t status;
            sp<IDirectTrack> track = createDirectTrack(pid,
                    sampleRate, channelMask, output, &sessionId, client,(audio_stream_type_t) streamType, &status);
            reply->writeInt32(sessionId);
            reply->writeInt32(status);
            reply->writeStrongBinder(track->asBinder());
            return NO_ERROR;
        } break;
#endif
        case OPEN_RECORD: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            audio_io_handle_t input = (audio_io_handle_t) data.readInt32();
            uint32_t sampleRate = data.readInt32();
            audio_format_t format = (audio_format_t) data.readInt32();
            audio_channel_mask_t channelMask = data.readInt32();
            size_t frameCount = data.readInt32();
            track_flags_t flags = (track_flags_t) data.readInt32();
            pid_t tid = (pid_t) data.readInt32();
            int sessionId = data.readInt32();
            status_t status;
            sp<IAudioRecord> record = openRecord(input,
                    sampleRate, format, channelMask, frameCount, &flags, tid, &sessionId, &status);
            LOG_ALWAYS_FATAL_IF((record != 0) != (status == NO_ERROR));
            reply->writeInt32(flags);
            reply->writeInt32(sessionId);
            reply->writeInt32(status);
            reply->writeStrongBinder(record->asBinder());
            return NO_ERROR;
        } break;
        case SAMPLE_RATE: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32( sampleRate((audio_io_handle_t) data.readInt32()) );
            return NO_ERROR;
        } break;
        case FORMAT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32( format((audio_io_handle_t) data.readInt32()) );
            return NO_ERROR;
        } break;
        case FRAME_COUNT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32( frameCount((audio_io_handle_t) data.readInt32()) );
            return NO_ERROR;
        } break;
        case LATENCY: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32( latency((audio_io_handle_t) data.readInt32()) );
            return NO_ERROR;
        } break;
        case SET_MASTER_VOLUME: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32( setMasterVolume(data.readFloat()) );
            return NO_ERROR;
        } break;
        case SET_MASTER_MUTE: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32( setMasterMute(data.readInt32()) );
            return NO_ERROR;
        } break;
        case MASTER_VOLUME: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeFloat( masterVolume() );
            return NO_ERROR;
        } break;
        case MASTER_MUTE: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32( masterMute() );
            return NO_ERROR;
        } break;
        case SET_STREAM_VOLUME: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            int stream = data.readInt32();
            float volume = data.readFloat();
            audio_io_handle_t output = (audio_io_handle_t) data.readInt32();
            reply->writeInt32( setStreamVolume((audio_stream_type_t) stream, volume, output) );
            return NO_ERROR;
        } break;
        case SET_STREAM_MUTE: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            int stream = data.readInt32();
            reply->writeInt32( setStreamMute((audio_stream_type_t) stream, data.readInt32()) );
            return NO_ERROR;
        } break;
        case STREAM_VOLUME: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            int stream = data.readInt32();
            int output = data.readInt32();
            reply->writeFloat( streamVolume((audio_stream_type_t) stream, output) );
            return NO_ERROR;
        } break;
        case STREAM_MUTE: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            int stream = data.readInt32();
            reply->writeInt32( streamMute((audio_stream_type_t) stream) );
            return NO_ERROR;
        } break;
        case SET_MODE: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            audio_mode_t mode = (audio_mode_t) data.readInt32();
            reply->writeInt32( setMode(mode) );
            return NO_ERROR;
        } break;
        case SET_MIC_MUTE: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            int state = data.readInt32();
            reply->writeInt32( setMicMute(state) );
            return NO_ERROR;
        } break;
        case GET_MIC_MUTE: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32( getMicMute() );
            return NO_ERROR;
        } break;
        case SET_PARAMETERS: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            audio_io_handle_t ioHandle = (audio_io_handle_t) data.readInt32();
            String8 keyValuePairs(data.readString8());
            reply->writeInt32(setParameters(ioHandle, keyValuePairs));
            return NO_ERROR;
        } break;
        case GET_PARAMETERS: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            audio_io_handle_t ioHandle = (audio_io_handle_t) data.readInt32();
            String8 keys(data.readString8());
            reply->writeString8(getParameters(ioHandle, keys));
            return NO_ERROR;
        } break;

        case REGISTER_CLIENT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            sp<IAudioFlingerClient> client = interface_cast<IAudioFlingerClient>(
                    data.readStrongBinder());
            registerClient(client);
            return NO_ERROR;
        } break;
        case GET_INPUTBUFFERSIZE: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            uint32_t sampleRate = data.readInt32();
            audio_format_t format = (audio_format_t) data.readInt32();
            audio_channel_mask_t channelMask = data.readInt32();
            reply->writeInt32( getInputBufferSize(sampleRate, format, channelMask) );
            return NO_ERROR;
        } break;
        case OPEN_OUTPUT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            audio_module_handle_t module = (audio_module_handle_t)data.readInt32();
            audio_devices_t devices = (audio_devices_t)data.readInt32();
            uint32_t samplingRate = data.readInt32();
            audio_format_t format = (audio_format_t) data.readInt32();
            audio_channel_mask_t channelMask = (audio_channel_mask_t)data.readInt32();
            uint32_t latency = data.readInt32();
            audio_output_flags_t flags = (audio_output_flags_t) data.readInt32();
            bool hasOffloadInfo = data.readInt32() != 0;
            audio_offload_info_t offloadInfo;
            if (hasOffloadInfo) {
                data.read(&offloadInfo, sizeof(audio_offload_info_t));
            }
            audio_io_handle_t output = openOutput(module,
                                                 &devices,
                                                 &samplingRate,
                                                 &format,
                                                 &channelMask,
                                                 &latency,
                                                 flags,
                                                 hasOffloadInfo ? &offloadInfo : NULL);
            ALOGV("OPEN_OUTPUT output, %p", output);
            reply->writeInt32((int32_t) output);
            reply->writeInt32(devices);
            reply->writeInt32(samplingRate);
            reply->writeInt32(format);
            reply->writeInt32(channelMask);
            reply->writeInt32(latency);
            return NO_ERROR;
        } break;
        case OPEN_DUPLICATE_OUTPUT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            audio_io_handle_t output1 = (audio_io_handle_t) data.readInt32();
            audio_io_handle_t output2 = (audio_io_handle_t) data.readInt32();
            reply->writeInt32((int32_t) openDuplicateOutput(output1, output2));
            return NO_ERROR;
        } break;
        case CLOSE_OUTPUT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32(closeOutput((audio_io_handle_t) data.readInt32()));
            return NO_ERROR;
        } break;
        case SUSPEND_OUTPUT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32(suspendOutput((audio_io_handle_t) data.readInt32()));
            return NO_ERROR;
        } break;
        case RESTORE_OUTPUT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32(restoreOutput((audio_io_handle_t) data.readInt32()));
            return NO_ERROR;
        } break;
        case OPEN_INPUT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            audio_module_handle_t module = (audio_module_handle_t)data.readInt32();
            audio_devices_t devices = (audio_devices_t)data.readInt32();
            uint32_t samplingRate = data.readInt32();
            audio_format_t format = (audio_format_t) data.readInt32();
            audio_channel_mask_t channelMask = (audio_channel_mask_t)data.readInt32();

            audio_io_handle_t input = openInput(module,
                                             &devices,
                                             &samplingRate,
                                             &format,
                                             &channelMask);
            reply->writeInt32((int32_t) input);
            reply->writeInt32(devices);
            reply->writeInt32(samplingRate);
            reply->writeInt32(format);
            reply->writeInt32(channelMask);
            return NO_ERROR;
        } break;
        case CLOSE_INPUT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32(closeInput((audio_io_handle_t) data.readInt32()));
            return NO_ERROR;
        } break;
        case SET_STREAM_OUTPUT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            uint32_t stream = data.readInt32();
            audio_io_handle_t output = (audio_io_handle_t) data.readInt32();
            reply->writeInt32(setStreamOutput((audio_stream_type_t) stream, output));
            return NO_ERROR;
        } break;
        case SET_VOICE_VOLUME: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            float volume = data.readFloat();
            reply->writeInt32( setVoiceVolume(volume) );
            return NO_ERROR;
        } break;
        case GET_RENDER_POSITION: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            audio_io_handle_t output = (audio_io_handle_t) data.readInt32();
            size_t halFrames;
            size_t dspFrames;
            status_t status = getRenderPosition(&halFrames, &dspFrames, output);
            reply->writeInt32(status);
            if (status == NO_ERROR) {
                reply->writeInt32(halFrames);
                reply->writeInt32(dspFrames);
            }
            return NO_ERROR;
        }
        case GET_INPUT_FRAMES_LOST: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            audio_io_handle_t ioHandle = (audio_io_handle_t) data.readInt32();
            reply->writeInt32(getInputFramesLost(ioHandle));
            return NO_ERROR;
        } break;
        case NEW_AUDIO_SESSION_ID: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32(newAudioSessionId());
            return NO_ERROR;
        } break;
        case ACQUIRE_AUDIO_SESSION_ID: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            int audioSession = data.readInt32();
            acquireAudioSessionId(audioSession);
            return NO_ERROR;
        } break;
        case RELEASE_AUDIO_SESSION_ID: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            int audioSession = data.readInt32();
            releaseAudioSessionId(audioSession);
            return NO_ERROR;
        } break;
        case QUERY_NUM_EFFECTS: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            uint32_t numEffects;
            status_t status = queryNumberEffects(&numEffects);
            reply->writeInt32(status);
            if (status == NO_ERROR) {
                reply->writeInt32((int32_t)numEffects);
            }
            return NO_ERROR;
        }
        case QUERY_EFFECT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            effect_descriptor_t desc;
            status_t status = queryEffect(data.readInt32(), &desc);
            reply->writeInt32(status);
            if (status == NO_ERROR) {
                reply->write(&desc, sizeof(effect_descriptor_t));
            }
            return NO_ERROR;
        }
        case GET_EFFECT_DESCRIPTOR: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            effect_uuid_t uuid;
            data.read(&uuid, sizeof(effect_uuid_t));
            effect_descriptor_t desc;
            status_t status = getEffectDescriptor(&uuid, &desc);
            reply->writeInt32(status);
            if (status == NO_ERROR) {
                reply->write(&desc, sizeof(effect_descriptor_t));
            }
            return NO_ERROR;
        }
        case CREATE_EFFECT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            effect_descriptor_t desc;
            data.read(&desc, sizeof(effect_descriptor_t));
            sp<IEffectClient> client = interface_cast<IEffectClient>(data.readStrongBinder());
            int32_t priority = data.readInt32();
            audio_io_handle_t output = (audio_io_handle_t) data.readInt32();
            int sessionId = data.readInt32();
            status_t status;
            int id;
            int enabled;

            sp<IEffect> effect = createEffect(&desc, client, priority, output, sessionId,
                    &status, &id, &enabled);
            reply->writeInt32(status);
            reply->writeInt32(id);
            reply->writeInt32(enabled);
            reply->writeStrongBinder(effect->asBinder());
            reply->write(&desc, sizeof(effect_descriptor_t));
            return NO_ERROR;
        } break;
        case MOVE_EFFECTS: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            int session = data.readInt32();
            audio_io_handle_t srcOutput = (audio_io_handle_t) data.readInt32();
            audio_io_handle_t dstOutput = (audio_io_handle_t) data.readInt32();
            reply->writeInt32(moveEffects(session, srcOutput, dstOutput));
            return NO_ERROR;
        } break;
        case LOAD_HW_MODULE: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32(loadHwModule(data.readCString()));
            return NO_ERROR;
        } break;
        case GET_PRIMARY_OUTPUT_SAMPLING_RATE: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32(getPrimaryOutputSamplingRate());
            return NO_ERROR;
        } break;
        case GET_PRIMARY_OUTPUT_FRAME_COUNT: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            reply->writeInt32(getPrimaryOutputFrameCount());
            return NO_ERROR;
        } break;
        case SET_LOW_RAM_DEVICE: {
            CHECK_INTERFACE(IAudioFlinger, data, reply);
            bool isLowRamDevice = data.readInt32() != 0;
            reply->writeInt32(setLowRamDevice(isLowRamDevice));
            return NO_ERROR;
        } break;
        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

// ----------------------------------------------------------------------------

}; // namespace android
