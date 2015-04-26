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

#ifndef ANDROID_AUDIOPOLICYSERVICE_H
#define ANDROID_AUDIOPOLICYSERVICE_H

#include <cutils/misc.h>
#include <cutils/config_utils.h>
#include <cutils/compiler.h>
#include <utils/String8.h>
#include <utils/Vector.h>
#include <utils/SortedVector.h>
#include <binder/BinderService.h>
#include <system/audio.h>
#include <system/audio_policy.h>
#include <hardware/audio_policy.h>
#include <hardware/power.h>
#include <media/IAudioPolicyService.h>
#include <media/ToneGenerator.h>
#include <media/AudioEffect.h>
#include <media/AudioPolicy.h>
#include <hardware_legacy/AudioPolicyInterface.h>
#include "AudioPolicyEffects.h"
#include "AudioPolicyManager.h"


namespace android {

// ----------------------------------------------------------------------------

class AudioPolicyService :
    public BinderService<AudioPolicyService>,
    public BnAudioPolicyService,
    public IBinder::DeathRecipient
{
    friend class BinderService<AudioPolicyService>;

public:
    // for BinderService
    static const char *getServiceName() ANDROID_API { return "media.audio_policy"; }

    virtual status_t    dump(int fd, const Vector<String16>& args);

    //
    // BnAudioPolicyService (see AudioPolicyInterface for method descriptions)
    //

    virtual status_t setDeviceConnectionState(audio_devices_t device,
                                              audio_policy_dev_state_t state,
                                              const char *device_address);
    virtual audio_policy_dev_state_t getDeviceConnectionState(
                                                                audio_devices_t device,
                                                                const char *device_address);
    virtual status_t setPhoneState(audio_mode_t state);
    virtual status_t setForceUse(audio_policy_force_use_t usage, audio_policy_forced_cfg_t config);
    virtual audio_policy_forced_cfg_t getForceUse(audio_policy_force_use_t usage);
    virtual audio_io_handle_t getOutput(audio_stream_type_t stream,
                                        uint32_t samplingRate = 0,
                                        audio_format_t format = AUDIO_FORMAT_DEFAULT,
                                        audio_channel_mask_t channelMask = 0,
                                        audio_output_flags_t flags =
                                                AUDIO_OUTPUT_FLAG_NONE,
                                        const audio_offload_info_t *offloadInfo = NULL);
    virtual status_t getOutputForAttr(const audio_attributes_t *attr,
                                      audio_io_handle_t *output,
                                      audio_session_t session,
                                      audio_stream_type_t *stream,
                                      uint32_t samplingRate = 0,
                                      audio_format_t format = AUDIO_FORMAT_DEFAULT,
                                      audio_channel_mask_t channelMask = 0,
                                      audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE,
                                      const audio_offload_info_t *offloadInfo = NULL);
    virtual status_t startOutput(audio_io_handle_t output,
                                 audio_stream_type_t stream,
                                 audio_session_t session);
    virtual status_t stopOutput(audio_io_handle_t output,
                                audio_stream_type_t stream,
                                audio_session_t session);
    virtual void releaseOutput(audio_io_handle_t output,
                               audio_stream_type_t stream,
                               audio_session_t session);
    virtual status_t getInputForAttr(const audio_attributes_t *attr,
                                     audio_io_handle_t *input,
                                     audio_session_t session,
                                     uint32_t samplingRate,
                                     audio_format_t format,
                                     audio_channel_mask_t channelMask,
                                     audio_input_flags_t flags);
    virtual status_t startInput(audio_io_handle_t input,
                                audio_session_t session);
    virtual status_t stopInput(audio_io_handle_t input,
                               audio_session_t session);
    virtual void releaseInput(audio_io_handle_t input,
                              audio_session_t session);
    virtual status_t initStreamVolume(audio_stream_type_t stream,
                                      int indexMin,
                                      int indexMax);
    virtual status_t setStreamVolumeIndex(audio_stream_type_t stream,
                                          int index,
                                          audio_devices_t device);
    virtual status_t getStreamVolumeIndex(audio_stream_type_t stream,
                                          int *index,
                                          audio_devices_t device);

    virtual uint32_t getStrategyForStream(audio_stream_type_t stream);
    virtual audio_devices_t getDevicesForStream(audio_stream_type_t stream);

    virtual audio_io_handle_t getOutputForEffect(const effect_descriptor_t *desc);
    virtual status_t registerEffect(const effect_descriptor_t *desc,
                                    audio_io_handle_t io,
                                    uint32_t strategy,
                                    int session,
                                    int id);
    virtual status_t unregisterEffect(int id);
    virtual status_t setEffectEnabled(int id, bool enabled);
    virtual bool isStreamActive(audio_stream_type_t stream, uint32_t inPastMs = 0) const;
    virtual bool isStreamActiveRemotely(audio_stream_type_t stream, uint32_t inPastMs = 0) const;
    virtual bool isSourceActive(audio_source_t source) const;

    virtual status_t queryDefaultPreProcessing(int audioSession,
                                              effect_descriptor_t *descriptors,
                                              uint32_t *count);
    virtual     status_t    onTransact(
                                uint32_t code,
                                const Parcel& data,
                                Parcel* reply,
                                uint32_t flags);

    // IBinder::DeathRecipient
    virtual     void        binderDied(const wp<IBinder>& who);

    // RefBase
    virtual     void        onFirstRef();

    //
    // Helpers for the struct audio_policy_service_ops implementation.
    // This is used by the audio policy manager for certain operations that
    // are implemented by the policy service.
    //
    virtual void setParameters(audio_io_handle_t ioHandle,
                               const char *keyValuePairs,
                               int delayMs);

    virtual status_t setStreamVolume(audio_stream_type_t stream,
                                     float volume,
                                     audio_io_handle_t output,
                                     int delayMs = 0);
    virtual status_t startTone(audio_policy_tone_t tone, audio_stream_type_t stream);
    virtual status_t stopTone();
    virtual status_t setVoiceVolume(float volume, int delayMs = 0);
    virtual bool isOffloadSupported(const audio_offload_info_t &config);

    virtual status_t listAudioPorts(audio_port_role_t role,
                                    audio_port_type_t type,
                                    unsigned int *num_ports,
                                    struct audio_port *ports,
                                    unsigned int *generation);
    virtual status_t getAudioPort(struct audio_port *port);
    virtual status_t createAudioPatch(const struct audio_patch *patch,
                                       audio_patch_handle_t *handle);
    virtual status_t releaseAudioPatch(audio_patch_handle_t handle);
    virtual status_t listAudioPatches(unsigned int *num_patches,
                                      struct audio_patch *patches,
                                      unsigned int *generation);
    virtual status_t setAudioPortConfig(const struct audio_port_config *config);

    virtual void registerClient(const sp<IAudioPolicyServiceClient>& client);

    virtual status_t acquireSoundTriggerSession(audio_session_t *session,
                                           audio_io_handle_t *ioHandle,
                                           audio_devices_t *device);

    virtual status_t releaseSoundTriggerSession(audio_session_t session);

    virtual audio_mode_t getPhoneState();

    virtual status_t registerPolicyMixes(Vector<AudioMix> mixes, bool registration);

            status_t doStopOutput(audio_io_handle_t output,
                                  audio_stream_type_t stream,
                                  audio_session_t session);
            void doReleaseOutput(audio_io_handle_t output,
                                 audio_stream_type_t stream,
                                 audio_session_t session);

            status_t clientCreateAudioPatch(const struct audio_patch *patch,
                                      audio_patch_handle_t *handle,
                                      int delayMs);
            status_t clientReleaseAudioPatch(audio_patch_handle_t handle,
                                             int delayMs);
            virtual status_t clientSetAudioPortConfig(const struct audio_port_config *config,
                                                      int delayMs);

            void removeNotificationClient(uid_t uid);
            void onAudioPortListUpdate();
            void doOnAudioPortListUpdate();
            void onAudioPatchListUpdate();
            void doOnAudioPatchListUpdate();

private:
                        AudioPolicyService() ANDROID_API;
    virtual             ~AudioPolicyService();

            status_t dumpInternals(int fd);

    // Thread used for tone playback and to send audio config commands to audio flinger
    // For tone playback, using a separate thread is necessary to avoid deadlock with mLock because
    // startTone() and stopTone() are normally called with mLock locked and requesting a tone start
    // or stop will cause calls to AudioPolicyService and an attempt to lock mLock.
    // For audio config commands, it is necessary because audio flinger requires that the calling
    // process (user) has permission to modify audio settings.
    class AudioCommandThread : public Thread {
        class AudioCommand;
    public:

        // commands for tone AudioCommand
        enum {
            START_TONE,
            STOP_TONE,
            SET_VOLUME,
            SET_PARAMETERS,
            SET_VOICE_VOLUME,
            STOP_OUTPUT,
            RELEASE_OUTPUT,
            CREATE_AUDIO_PATCH,
            RELEASE_AUDIO_PATCH,
            UPDATE_AUDIOPORT_LIST,
            UPDATE_AUDIOPATCH_LIST,
            SET_AUDIOPORT_CONFIG,
        };

        AudioCommandThread (String8 name, const wp<AudioPolicyService>& service);
        virtual             ~AudioCommandThread();

                    status_t    dump(int fd);

        // Thread virtuals
        virtual     void        onFirstRef();
        virtual     bool        threadLoop();

                    void        exit();
                    void        startToneCommand(ToneGenerator::tone_type type,
                                                 audio_stream_type_t stream);
                    void        stopToneCommand();
                    status_t    volumeCommand(audio_stream_type_t stream, float volume,
                                            audio_io_handle_t output, int delayMs = 0);
                    status_t    parametersCommand(audio_io_handle_t ioHandle,
                                            const char *keyValuePairs, int delayMs = 0);
                    status_t    voiceVolumeCommand(float volume, int delayMs = 0);
                    void        stopOutputCommand(audio_io_handle_t output,
                                                  audio_stream_type_t stream,
                                                  audio_session_t session);
                    void        releaseOutputCommand(audio_io_handle_t output,
                                                     audio_stream_type_t stream,
                                                     audio_session_t session);
                    status_t    sendCommand(sp<AudioCommand>& command, int delayMs = 0);
                    void        insertCommand_l(sp<AudioCommand>& command, int delayMs = 0);
                    status_t    createAudioPatchCommand(const struct audio_patch *patch,
                                                        audio_patch_handle_t *handle,
                                                        int delayMs);
                    status_t    releaseAudioPatchCommand(audio_patch_handle_t handle,
                                                         int delayMs);
                    void        updateAudioPortListCommand();
                    void        updateAudioPatchListCommand();
                    status_t    setAudioPortConfigCommand(const struct audio_port_config *config,
                                                          int delayMs);
                    void        insertCommand_l(AudioCommand *command, int delayMs = 0);

    private:
        class AudioCommandData;

        // descriptor for requested tone playback event
        class AudioCommand: public RefBase {

        public:
            AudioCommand()
            : mCommand(-1), mStatus(NO_ERROR), mWaitStatus(false) {}

            void dump(char* buffer, size_t size);

            int mCommand;   // START_TONE, STOP_TONE ...
            nsecs_t mTime;  // time stamp
            Mutex mLock;    // mutex associated to mCond
            Condition mCond; // condition for status return
            status_t mStatus; // command status
            bool mWaitStatus; // true if caller is waiting for status
            sp<AudioCommandData> mParam;     // command specific parameter data
        };

        class AudioCommandData: public RefBase {
        public:
            virtual ~AudioCommandData() {}
        protected:
            AudioCommandData() {}
        };

        class ToneData : public AudioCommandData {
        public:
            ToneGenerator::tone_type mType; // tone type (START_TONE only)
            audio_stream_type_t mStream;    // stream type (START_TONE only)
        };

        class VolumeData : public AudioCommandData {
        public:
            audio_stream_type_t mStream;
            float mVolume;
            audio_io_handle_t mIO;
        };

        class ParametersData : public AudioCommandData {
        public:
            audio_io_handle_t mIO;
            String8 mKeyValuePairs;
        };

        class VoiceVolumeData : public AudioCommandData {
        public:
            float mVolume;
        };

        class StopOutputData : public AudioCommandData {
        public:
            audio_io_handle_t mIO;
            audio_stream_type_t mStream;
            audio_session_t mSession;
        };

        class ReleaseOutputData : public AudioCommandData {
        public:
            audio_io_handle_t mIO;
            audio_stream_type_t mStream;
            audio_session_t mSession;
        };

        class CreateAudioPatchData : public AudioCommandData {
        public:
            struct audio_patch mPatch;
            audio_patch_handle_t mHandle;
        };

        class ReleaseAudioPatchData : public AudioCommandData {
        public:
            audio_patch_handle_t mHandle;
        };

        class SetAudioPortConfigData : public AudioCommandData {
        public:
            struct audio_port_config mConfig;
        };

        Mutex   mLock;
        Condition mWaitWorkCV;
        Vector < sp<AudioCommand> > mAudioCommands; // list of pending commands
        ToneGenerator *mpToneGenerator;     // the tone generator
        sp<AudioCommand> mLastCommand;      // last processed command (used by dump)
        String8 mName;                      // string used by wake lock fo delayed commands
        wp<AudioPolicyService> mService;
    };

    class AudioPolicyClient : public AudioPolicyClientInterface
    {
     public:
        AudioPolicyClient(AudioPolicyService *service) : mAudioPolicyService(service) {}
        virtual ~AudioPolicyClient() {}

        //
        // Audio HW module functions
        //

        // loads a HW module.
        virtual audio_module_handle_t loadHwModule(const char *name);

        //
        // Audio output Control functions
        //

        // opens an audio output with the requested parameters. The parameter values can indicate to use the default values
        // in case the audio policy manager has no specific requirements for the output being opened.
        // When the function returns, the parameter values reflect the actual values used by the audio hardware output stream.
        // The audio policy manager can check if the proposed parameters are suitable or not and act accordingly.
        virtual status_t openOutput(audio_module_handle_t module,
                                    audio_io_handle_t *output,
                                    audio_config_t *config,
                                    audio_devices_t *devices,
                                    const String8& address,
                                    uint32_t *latencyMs,
                                    audio_output_flags_t flags);
        // creates a special output that is duplicated to the two outputs passed as arguments. The duplication is performed by
        // a special mixer thread in the AudioFlinger.
        virtual audio_io_handle_t openDuplicateOutput(audio_io_handle_t output1, audio_io_handle_t output2);
        // closes the output stream
        virtual status_t closeOutput(audio_io_handle_t output);
        // suspends the output. When an output is suspended, the corresponding audio hardware output stream is placed in
        // standby and the AudioTracks attached to the mixer thread are still processed but the output mix is discarded.
        virtual status_t suspendOutput(audio_io_handle_t output);
        // restores a suspended output.
        virtual status_t restoreOutput(audio_io_handle_t output);

        //
        // Audio input Control functions
        //

        // opens an audio input
        virtual audio_io_handle_t openInput(audio_module_handle_t module,
                                            audio_io_handle_t *input,
                                            audio_config_t *config,
                                            audio_devices_t *devices,
                                            const String8& address,
                                            audio_source_t source,
                                            audio_input_flags_t flags);
        // closes an audio input
        virtual status_t closeInput(audio_io_handle_t input);
        //
        // misc control functions
        //

        // set a stream volume for a particular output. For the same user setting, a given stream type can have different volumes
        // for each output (destination device) it is attached to.
        virtual status_t setStreamVolume(audio_stream_type_t stream, float volume, audio_io_handle_t output, int delayMs = 0);

        // invalidate a stream type, causing a reroute to an unspecified new output
        virtual status_t invalidateStream(audio_stream_type_t stream);

        // function enabling to send proprietary informations directly from audio policy manager to audio hardware interface.
        virtual void setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs, int delayMs = 0);
        // function enabling to receive proprietary informations directly from audio hardware interface to audio policy manager.
        virtual String8 getParameters(audio_io_handle_t ioHandle, const String8& keys);

        // request the playback of a tone on the specified stream: used for instance to replace notification sounds when playing
        // over a telephony device during a phone call.
        virtual status_t startTone(audio_policy_tone_t tone, audio_stream_type_t stream);
        virtual status_t stopTone();

        // set down link audio volume.
        virtual status_t setVoiceVolume(float volume, int delayMs = 0);

        // move effect to the specified output
        virtual status_t moveEffects(int session,
                                         audio_io_handle_t srcOutput,
                                         audio_io_handle_t dstOutput);

        /* Create a patch between several source and sink ports */
        virtual status_t createAudioPatch(const struct audio_patch *patch,
                                           audio_patch_handle_t *handle,
                                           int delayMs);

        /* Release a patch */
        virtual status_t releaseAudioPatch(audio_patch_handle_t handle,
                                           int delayMs);

        /* Set audio port configuration */
        virtual status_t setAudioPortConfig(const struct audio_port_config *config, int delayMs);

        virtual void onAudioPortListUpdate();
        virtual void onAudioPatchListUpdate();

        virtual audio_unique_id_t newAudioUniqueId();

     private:
        AudioPolicyService *mAudioPolicyService;
    };

    // --- Notification Client ---
    class NotificationClient : public IBinder::DeathRecipient {
    public:
                            NotificationClient(const sp<AudioPolicyService>& service,
                                                const sp<IAudioPolicyServiceClient>& client,
                                                uid_t uid);
        virtual             ~NotificationClient();

                            void        onAudioPortListUpdate();
                            void        onAudioPatchListUpdate();

                // IBinder::DeathRecipient
                virtual     void        binderDied(const wp<IBinder>& who);

    private:
                            NotificationClient(const NotificationClient&);
                            NotificationClient& operator = (const NotificationClient&);

        const wp<AudioPolicyService>        mService;
        const uid_t                         mUid;
        const sp<IAudioPolicyServiceClient> mAudioPolicyServiceClient;
    };

    // Internal dump utilities.
    status_t dumpPermissionDenial(int fd);

    void setPowerHint(bool active);

    mutable Mutex mLock;    // prevents concurrent access to AudioPolicy manager functions changing
                            // device connection state  or routing
    sp<AudioCommandThread> mAudioCommandThread;     // audio commands thread
    sp<AudioCommandThread> mTonePlaybackThread;     // tone playback thread
    sp<AudioCommandThread> mOutputCommandThread;    // process stop and release output
    struct audio_policy_device *mpAudioPolicyDev;
    struct audio_policy *mpAudioPolicy;
    AudioPolicyInterface *mAudioPolicyManager;
    AudioPolicyClient *mAudioPolicyClient;

    DefaultKeyedVector< uid_t, sp<NotificationClient> >    mNotificationClients;
    Mutex mNotificationClientsLock;  // protects mNotificationClients
    // Manage all effects configured in audio_effects.conf
    sp<AudioPolicyEffects> mAudioPolicyEffects;
    audio_mode_t mPhoneState;

    power_module_t *mPowerModule;
};

}; // namespace android

#endif // ANDROID_AUDIOPOLICYSERVICE_H
