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


#include <stdint.h>
#include <sys/types.h>
#include <cutils/config_utils.h>
#include <cutils/misc.h>
#include <utils/Timers.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/SortedVector.h>
#include "AudioPolicyInterface.h"


namespace android {

// ----------------------------------------------------------------------------

// Attenuation applied to STRATEGY_SONIFICATION streams when a headset is connected: 6dB
#define SONIFICATION_HEADSET_VOLUME_FACTOR 0.5
// Min volume for STRATEGY_SONIFICATION streams when limited by music volume: -36dB
#define SONIFICATION_HEADSET_VOLUME_MIN  0.016
// Time in milliseconds during which we consider that music is still active after a music
// track was stopped - see computeVolume()
#define SONIFICATION_HEADSET_MUSIC_DELAY  5000
// Time in milliseconds after media stopped playing during which we consider that the
// sonification should be as unobtrusive as during the time media was playing.
#define SONIFICATION_RESPECTFUL_AFTER_MUSIC_DELAY 5000
// Time in milliseconds during witch some streams are muted while the audio path
// is switched
#define MUTE_TIME_MS 2000

#define NUM_TEST_OUTPUTS 5

#define NUM_VOL_CURVE_KNEES 2

// Default minimum length allowed for offloading a compressed track
// Can be overridden by the audio.offload.min.duration.secs property
#define OFFLOAD_DEFAULT_MIN_DURATION_SECS 60

#define MAX_MIXER_SAMPLING_RATE 48000
#define MAX_MIXER_CHANNEL_COUNT 8

// ----------------------------------------------------------------------------
// AudioPolicyManager implements audio policy manager behavior common to all platforms.
// ----------------------------------------------------------------------------

class AudioPolicyManager: public AudioPolicyInterface
#ifdef AUDIO_POLICY_TEST
    , public Thread
#endif //AUDIO_POLICY_TEST
{

public:
                AudioPolicyManager(AudioPolicyClientInterface *clientInterface);
        virtual ~AudioPolicyManager();

        // AudioPolicyInterface
        virtual status_t setDeviceConnectionState(audio_devices_t device,
                                                          audio_policy_dev_state_t state,
                                                          const char *device_address);
        virtual audio_policy_dev_state_t getDeviceConnectionState(audio_devices_t device,
                                                                              const char *device_address);
        virtual void setPhoneState(audio_mode_t state);
        virtual void setForceUse(audio_policy_force_use_t usage,
                                 audio_policy_forced_cfg_t config);
        virtual audio_policy_forced_cfg_t getForceUse(audio_policy_force_use_t usage);
        virtual void setSystemProperty(const char* property, const char* value);
        virtual status_t initCheck();
        virtual audio_io_handle_t getOutput(audio_stream_type_t stream,
                                            uint32_t samplingRate,
                                            audio_format_t format,
                                            audio_channel_mask_t channelMask,
                                            audio_output_flags_t flags,
                                            const audio_offload_info_t *offloadInfo);
        virtual audio_io_handle_t getOutputForAttr(const audio_attributes_t *attr,
                                            uint32_t samplingRate,
                                            audio_format_t format,
                                            audio_channel_mask_t channelMask,
                                            audio_output_flags_t flags,
                                            const audio_offload_info_t *offloadInfo);
        virtual status_t startOutput(audio_io_handle_t output,
                                     audio_stream_type_t stream,
                                     int session = 0);
        virtual status_t stopOutput(audio_io_handle_t output,
                                    audio_stream_type_t stream,
                                    int session = 0);
        virtual void releaseOutput(audio_io_handle_t output);
        virtual audio_io_handle_t getInput(audio_source_t inputSource,
                                            uint32_t samplingRate,
                                            audio_format_t format,
                                            audio_channel_mask_t channelMask,
                                            audio_session_t session,
                                            audio_input_flags_t flags);

        // indicates to the audio policy manager that the input starts being used.
        virtual status_t startInput(audio_io_handle_t input,
                                    audio_session_t session);

        // indicates to the audio policy manager that the input stops being used.
        virtual status_t stopInput(audio_io_handle_t input,
                                   audio_session_t session);
        virtual void releaseInput(audio_io_handle_t input,
                                  audio_session_t session);
        virtual void closeAllInputs();
        virtual void initStreamVolume(audio_stream_type_t stream,
                                                    int indexMin,
                                                    int indexMax);
        virtual status_t setStreamVolumeIndex(audio_stream_type_t stream,
                                              int index,
                                              audio_devices_t device);
        virtual status_t getStreamVolumeIndex(audio_stream_type_t stream,
                                              int *index,
                                              audio_devices_t device);

        // return the strategy corresponding to a given stream type
        virtual uint32_t getStrategyForStream(audio_stream_type_t stream);
        // return the strategy corresponding to the given audio attributes
        virtual uint32_t getStrategyForAttr(const audio_attributes_t *attr);

        // return the enabled output devices for the given stream type
        virtual audio_devices_t getDevicesForStream(audio_stream_type_t stream);

        virtual audio_io_handle_t getOutputForEffect(const effect_descriptor_t *desc = NULL);
        virtual status_t registerEffect(const effect_descriptor_t *desc,
                                        audio_io_handle_t io,
                                        uint32_t strategy,
                                        int session,
                                        int id);
        virtual status_t unregisterEffect(int id);
        virtual status_t setEffectEnabled(int id, bool enabled);

        virtual bool isStreamActive(audio_stream_type_t stream, uint32_t inPastMs = 0) const;
        // return whether a stream is playing remotely, override to change the definition of
        //   local/remote playback, used for instance by notification manager to not make
        //   media players lose audio focus when not playing locally
        virtual bool isStreamActiveRemotely(audio_stream_type_t stream, uint32_t inPastMs = 0) const;
        virtual bool isSourceActive(audio_source_t source) const;

        virtual status_t dump(int fd);

        virtual bool isOffloadSupported(const audio_offload_info_t& offloadInfo);

        virtual status_t listAudioPorts(audio_port_role_t role,
                                        audio_port_type_t type,
                                        unsigned int *num_ports,
                                        struct audio_port *ports,
                                        unsigned int *generation);
        virtual status_t getAudioPort(struct audio_port *port);
        virtual status_t createAudioPatch(const struct audio_patch *patch,
                                           audio_patch_handle_t *handle,
                                           uid_t uid);
        virtual status_t releaseAudioPatch(audio_patch_handle_t handle,
                                              uid_t uid);
        virtual status_t listAudioPatches(unsigned int *num_patches,
                                          struct audio_patch *patches,
                                          unsigned int *generation);
        virtual status_t setAudioPortConfig(const struct audio_port_config *config);
        virtual void clearAudioPatches(uid_t uid);

        virtual status_t acquireSoundTriggerSession(audio_session_t *session,
                                               audio_io_handle_t *ioHandle,
                                               audio_devices_t *device);

        virtual status_t releaseSoundTriggerSession(audio_session_t session);

protected:

        enum routing_strategy {
            STRATEGY_MEDIA,
            STRATEGY_PHONE,
            STRATEGY_SONIFICATION,
            STRATEGY_SONIFICATION_RESPECTFUL,
            STRATEGY_DTMF,
            STRATEGY_ENFORCED_AUDIBLE,
            NUM_STRATEGIES
        };

        // 4 points to define the volume attenuation curve, each characterized by the volume
        // index (from 0 to 100) at which they apply, and the attenuation in dB at that index.
        // we use 100 steps to avoid rounding errors when computing the volume in volIndexToAmpl()

        enum { VOLMIN = 0, VOLKNEE1 = 1, VOLKNEE2 = 2, VOLMAX = 3, VOLCNT = 4};

        class VolumeCurvePoint
        {
        public:
            int mIndex;
            float mDBAttenuation;
        };

        // device categories used for volume curve management.
        enum device_category {
            DEVICE_CATEGORY_HEADSET,
            DEVICE_CATEGORY_SPEAKER,
            DEVICE_CATEGORY_EARPIECE,
            DEVICE_CATEGORY_EXT_MEDIA,
            DEVICE_CATEGORY_CNT
        };

        class HwModule;

        class AudioGain: public RefBase
        {
        public:
            AudioGain(int index, bool useInChannelMask);
            virtual ~AudioGain() {}

            void dump(int fd, int spaces, int index) const;

            void getDefaultConfig(struct audio_gain_config *config);
            status_t checkConfig(const struct audio_gain_config *config);
            int               mIndex;
            struct audio_gain mGain;
            bool              mUseInChannelMask;
        };

        class AudioPort: public virtual RefBase
        {
        public:
            AudioPort(const String8& name, audio_port_type_t type,
                      audio_port_role_t role, const sp<HwModule>& module);
            virtual ~AudioPort() {}

            virtual void toAudioPort(struct audio_port *port) const;

            void importAudioPort(const sp<AudioPort> port);
            void clearCapabilities();

            void loadSamplingRates(char *name);
            void loadFormats(char *name);
            void loadOutChannels(char *name);
            void loadInChannels(char *name);

            audio_gain_mode_t loadGainMode(char *name);
            void loadGain(cnode *root, int index);
            void loadGains(cnode *root);

            // searches for an exact match
            status_t checkExactSamplingRate(uint32_t samplingRate) const;
            // searches for a compatible match, and returns the best match via updatedSamplingRate
            status_t checkCompatibleSamplingRate(uint32_t samplingRate,
                    uint32_t *updatedSamplingRate) const;
            // searches for an exact match
            status_t checkExactChannelMask(audio_channel_mask_t channelMask) const;
            // searches for a compatible match, currently implemented for input channel masks only
            status_t checkCompatibleChannelMask(audio_channel_mask_t channelMask) const;
            status_t checkFormat(audio_format_t format) const;
            status_t checkGain(const struct audio_gain_config *gainConfig, int index) const;

            uint32_t pickSamplingRate() const;
            audio_channel_mask_t pickChannelMask() const;
            audio_format_t pickFormat() const;

            static const audio_format_t sPcmFormatCompareTable[];
            static int compareFormats(audio_format_t format1, audio_format_t format2);

            void dump(int fd, int spaces) const;

            String8           mName;
            audio_port_type_t mType;
            audio_port_role_t mRole;
            bool              mUseInChannelMask;
            // by convention, "0' in the first entry in mSamplingRates, mChannelMasks or mFormats
            // indicates the supported parameters should be read from the output stream
            // after it is opened for the first time
            Vector <uint32_t> mSamplingRates; // supported sampling rates
            Vector <audio_channel_mask_t> mChannelMasks; // supported channel masks
            Vector <audio_format_t> mFormats; // supported audio formats
            Vector < sp<AudioGain> > mGains; // gain controllers
            sp<HwModule> mModule;                 // audio HW module exposing this I/O stream
            audio_output_flags_t mFlags; // attribute flags (e.g primary output,
                                                // direct output...). For outputs only.
        };

        class AudioPortConfig: public virtual RefBase
        {
        public:
            AudioPortConfig();
            virtual ~AudioPortConfig() {}

            status_t applyAudioPortConfig(const struct audio_port_config *config,
                                          struct audio_port_config *backupConfig = NULL);
            virtual void toAudioPortConfig(struct audio_port_config *dstConfig,
                                   const struct audio_port_config *srcConfig = NULL) const = 0;
            virtual sp<AudioPort> getAudioPort() const = 0;
            uint32_t mSamplingRate;
            audio_format_t mFormat;
            audio_channel_mask_t mChannelMask;
            struct audio_gain_config mGain;
        };


        class AudioPatch: public RefBase
        {
        public:
            AudioPatch(audio_patch_handle_t handle,
                       const struct audio_patch *patch, uid_t uid) :
                           mHandle(handle), mPatch(*patch), mUid(uid), mAfPatchHandle(0) {}

            status_t dump(int fd, int spaces, int index) const;

            audio_patch_handle_t mHandle;
            struct audio_patch mPatch;
            uid_t mUid;
            audio_patch_handle_t mAfPatchHandle;
        };

        class DeviceDescriptor: public AudioPort, public AudioPortConfig
        {
        public:
            DeviceDescriptor(const String8& name, audio_devices_t type);

            virtual ~DeviceDescriptor() {}

            bool equals(const sp<DeviceDescriptor>& other) const;
            virtual void toAudioPortConfig(struct audio_port_config *dstConfig,
                                   const struct audio_port_config *srcConfig = NULL) const;
            virtual sp<AudioPort> getAudioPort() const { return (AudioPort*) this; }

            virtual void toAudioPort(struct audio_port *port) const;

            status_t dump(int fd, int spaces, int index) const;

            audio_devices_t mDeviceType;
            String8 mAddress;
            audio_port_handle_t mId;
        };

        class DeviceVector : public SortedVector< sp<DeviceDescriptor> >
        {
        public:
            DeviceVector() : SortedVector(), mDeviceTypes(AUDIO_DEVICE_NONE) {}

            ssize_t         add(const sp<DeviceDescriptor>& item);
            ssize_t         remove(const sp<DeviceDescriptor>& item);
            ssize_t         indexOf(const sp<DeviceDescriptor>& item) const;

            audio_devices_t types() const { return mDeviceTypes; }

            void loadDevicesFromType(audio_devices_t types);
            void loadDevicesFromName(char *name, const DeviceVector& declaredDevices);

            sp<DeviceDescriptor> getDevice(audio_devices_t type, String8 address) const;
            DeviceVector getDevicesFromType(audio_devices_t types) const;
            sp<DeviceDescriptor> getDeviceFromId(audio_port_handle_t id) const;
            sp<DeviceDescriptor> getDeviceFromName(const String8& name) const;
            DeviceVector getDevicesFromTypeAddr(audio_devices_t type, String8 address)
                    const;

        private:
            void refreshTypes();
            audio_devices_t mDeviceTypes;
        };

        // the IOProfile class describes the capabilities of an output or input stream.
        // It is currently assumed that all combination of listed parameters are supported.
        // It is used by the policy manager to determine if an output or input is suitable for
        // a given use case,  open/close it accordingly and connect/disconnect audio tracks
        // to/from it.
        class IOProfile : public AudioPort
        {
        public:
            IOProfile(const String8& name, audio_port_role_t role, const sp<HwModule>& module);
            virtual ~IOProfile();

            // This method is used for both output and input.
            // If parameter updatedSamplingRate is non-NULL, it is assigned the actual sample rate.
            // For input, flags is interpreted as audio_input_flags_t.
            // TODO: merge audio_output_flags_t and audio_input_flags_t.
            bool isCompatibleProfile(audio_devices_t device,
                                     uint32_t samplingRate,
                                     uint32_t *updatedSamplingRate,
                                     audio_format_t format,
                                     audio_channel_mask_t channelMask,
                                     audio_output_flags_t flags) const;

            void dump(int fd);
            void log();

            DeviceVector  mSupportedDevices; // supported devices
                                             // (devices this output can be routed to)
        };

        class HwModule : public RefBase
        {
        public:
                    HwModule(const char *name);
                    ~HwModule();

            status_t loadOutput(cnode *root);
            status_t loadInput(cnode *root);
            status_t loadDevice(cnode *root);

            void dump(int fd);

            const char *const        mName; // base name of the audio HW module (primary, a2dp ...)
            uint32_t                 mHalVersion; // audio HAL API version
            audio_module_handle_t    mHandle;
            Vector < sp<IOProfile> > mOutputProfiles; // output profiles exposed by this module
            Vector < sp<IOProfile> > mInputProfiles;  // input profiles exposed by this module
            DeviceVector             mDeclaredDevices; // devices declared in audio_policy.conf

        };

        // default volume curve
        static const VolumeCurvePoint sDefaultVolumeCurve[AudioPolicyManager::VOLCNT];
        // default volume curve for media strategy
        static const VolumeCurvePoint sDefaultMediaVolumeCurve[AudioPolicyManager::VOLCNT];
        // volume curve for non-media audio on ext media outputs (HDMI, Line, etc)
        static const VolumeCurvePoint sExtMediaSystemVolumeCurve[AudioPolicyManager::VOLCNT];
        // volume curve for media strategy on speakers
        static const VolumeCurvePoint sSpeakerMediaVolumeCurve[AudioPolicyManager::VOLCNT];
        static const VolumeCurvePoint sSpeakerMediaVolumeCurveDrc[AudioPolicyManager::VOLCNT];
        // volume curve for sonification strategy on speakers
        static const VolumeCurvePoint sSpeakerSonificationVolumeCurve[AudioPolicyManager::VOLCNT];
        static const VolumeCurvePoint sSpeakerSonificationVolumeCurveDrc[AudioPolicyManager::VOLCNT];
        static const VolumeCurvePoint sDefaultSystemVolumeCurve[AudioPolicyManager::VOLCNT];
        static const VolumeCurvePoint sDefaultSystemVolumeCurveDrc[AudioPolicyManager::VOLCNT];
        static const VolumeCurvePoint sHeadsetSystemVolumeCurve[AudioPolicyManager::VOLCNT];
        static const VolumeCurvePoint sDefaultVoiceVolumeCurve[AudioPolicyManager::VOLCNT];
        static const VolumeCurvePoint sSpeakerVoiceVolumeCurve[AudioPolicyManager::VOLCNT];
        // default volume curves per stream and device category. See initializeVolumeCurves()
        static const VolumeCurvePoint *sVolumeProfiles[AUDIO_STREAM_CNT][DEVICE_CATEGORY_CNT];

        // descriptor for audio outputs. Used to maintain current configuration of each opened audio output
        // and keep track of the usage of this output by each audio stream type.
        class AudioOutputDescriptor: public AudioPortConfig
        {
        public:
            AudioOutputDescriptor(const sp<IOProfile>& profile);

            status_t    dump(int fd);

            audio_devices_t device() const;
            void changeRefCount(audio_stream_type_t stream, int delta);

            bool isDuplicated() const { return (mOutput1 != NULL && mOutput2 != NULL); }
            audio_devices_t supportedDevices();
            uint32_t latency();
            bool sharesHwModuleWith(const sp<AudioOutputDescriptor> outputDesc);
            bool isActive(uint32_t inPastMs = 0) const;
            bool isStreamActive(audio_stream_type_t stream,
                                uint32_t inPastMs = 0,
                                nsecs_t sysTime = 0) const;
            bool isStrategyActive(routing_strategy strategy,
                             uint32_t inPastMs = 0,
                             nsecs_t sysTime = 0) const;

            virtual void toAudioPortConfig(struct audio_port_config *dstConfig,
                                   const struct audio_port_config *srcConfig = NULL) const;
            virtual sp<AudioPort> getAudioPort() const { return mProfile; }
            void toAudioPort(struct audio_port *port) const;

            audio_port_handle_t mId;
            audio_io_handle_t mIoHandle;              // output handle
            uint32_t mLatency;                  //
            audio_output_flags_t mFlags;   //
            audio_devices_t mDevice;                   // current device this output is routed to
            audio_patch_handle_t mPatchHandle;
            uint32_t mRefCount[AUDIO_STREAM_CNT]; // number of streams of each type using this output
            nsecs_t mStopTime[AUDIO_STREAM_CNT];
            sp<AudioOutputDescriptor> mOutput1;    // used by duplicated outputs: first output
            sp<AudioOutputDescriptor> mOutput2;    // used by duplicated outputs: second output
            float mCurVolume[AUDIO_STREAM_CNT];   // current stream volume
            int mMuteCount[AUDIO_STREAM_CNT];     // mute request counter
            const sp<IOProfile> mProfile;          // I/O profile this output derives from
            bool mStrategyMutedByDevice[NUM_STRATEGIES]; // strategies muted because of incompatible
                                                // device selection. See checkDeviceMuteStrategies()
            uint32_t mDirectOpenCount; // number of clients using this output (direct outputs only)
        };

        // descriptor for audio inputs. Used to maintain current configuration of each opened audio input
        // and keep track of the usage of this input.
        class AudioInputDescriptor: public AudioPortConfig
        {
        public:
            AudioInputDescriptor(const sp<IOProfile>& profile);

            status_t    dump(int fd);

            audio_port_handle_t           mId;
            audio_io_handle_t             mIoHandle;       // input handle
            audio_devices_t               mDevice;         // current device this input is routed to
            audio_patch_handle_t          mPatchHandle;
            uint32_t                      mRefCount;       // number of AudioRecord clients using
                                                           // this input
            uint32_t                      mOpenRefCount;
            audio_source_t                mInputSource;    // input source selected by application
                                                           //(mediarecorder.h)
            const sp<IOProfile>           mProfile;        // I/O profile this output derives from
            SortedVector<audio_session_t> mSessions;       // audio sessions attached to this input
            bool                          mIsSoundTrigger; // used by a soundtrigger capture

            virtual void toAudioPortConfig(struct audio_port_config *dstConfig,
                                   const struct audio_port_config *srcConfig = NULL) const;
            virtual sp<AudioPort> getAudioPort() const { return mProfile; }
            void toAudioPort(struct audio_port *port) const;
        };

        // stream descriptor used for volume control
        class StreamDescriptor
        {
        public:
            StreamDescriptor();

            int getVolumeIndex(audio_devices_t device);
            void dump(int fd);

            int mIndexMin;      // min volume index
            int mIndexMax;      // max volume index
            KeyedVector<audio_devices_t, int> mIndexCur;   // current volume index per device
            bool mCanBeMuted;   // true is the stream can be muted

            const VolumeCurvePoint *mVolumeCurve[DEVICE_CATEGORY_CNT];
        };

        // stream descriptor used for volume control
        class EffectDescriptor : public RefBase
        {
        public:

            status_t dump(int fd);

            int mIo;                // io the effect is attached to
            routing_strategy mStrategy; // routing strategy the effect is associated to
            int mSession;               // audio session the effect is on
            effect_descriptor_t mDesc;  // effect descriptor
            bool mEnabled;              // enabled state: CPU load being used or not
        };

        void addOutput(audio_io_handle_t output, sp<AudioOutputDescriptor> outputDesc);
        void addInput(audio_io_handle_t input, sp<AudioInputDescriptor> inputDesc);

        // return the strategy corresponding to a given stream type
        static routing_strategy getStrategy(audio_stream_type_t stream);

        // return appropriate device for streams handled by the specified strategy according to current
        // phone state, connected devices...
        // if fromCache is true, the device is returned from mDeviceForStrategy[],
        // otherwise it is determine by current state
        // (device connected,phone state, force use, a2dp output...)
        // This allows to:
        //  1 speed up process when the state is stable (when starting or stopping an output)
        //  2 access to either current device selection (fromCache == true) or
        // "future" device selection (fromCache == false) when called from a context
        //  where conditions are changing (setDeviceConnectionState(), setPhoneState()...) AND
        //  before updateDevicesAndOutputs() is called.
        virtual audio_devices_t getDeviceForStrategy(routing_strategy strategy,
                                                     bool fromCache);

        // change the route of the specified output. Returns the number of ms we have slept to
        // allow new routing to take effect in certain cases.
        uint32_t setOutputDevice(audio_io_handle_t output,
                             audio_devices_t device,
                             bool force = false,
                             int delayMs = 0,
                             audio_patch_handle_t *patchHandle = NULL,
                             const char* address = NULL);
        status_t resetOutputDevice(audio_io_handle_t output,
                                   int delayMs = 0,
                                   audio_patch_handle_t *patchHandle = NULL);
        status_t setInputDevice(audio_io_handle_t input,
                                audio_devices_t device,
                                bool force = false,
                                audio_patch_handle_t *patchHandle = NULL);
        status_t resetInputDevice(audio_io_handle_t input,
                                  audio_patch_handle_t *patchHandle = NULL);

        // select input device corresponding to requested audio source
        virtual audio_devices_t getDeviceForInputSource(audio_source_t inputSource);

        // return io handle of active input or 0 if no input is active
        //    Only considers inputs from physical devices (e.g. main mic, headset mic) when
        //    ignoreVirtualInputs is true.
        audio_io_handle_t getActiveInput(bool ignoreVirtualInputs = true);

        uint32_t activeInputsCount() const;

        // initialize volume curves for each strategy and device category
        void initializeVolumeCurves();

        // compute the actual volume for a given stream according to the requested index and a particular
        // device
        virtual float computeVolume(audio_stream_type_t stream, int index,
                                    audio_io_handle_t output, audio_devices_t device);

        // check that volume change is permitted, compute and send new volume to audio hardware
        status_t checkAndSetVolume(audio_stream_type_t stream, int index, audio_io_handle_t output,
                                   audio_devices_t device, int delayMs = 0, bool force = false);

        // apply all stream volumes to the specified output and device
        void applyStreamVolumes(audio_io_handle_t output, audio_devices_t device, int delayMs = 0, bool force = false);

        // Mute or unmute all streams handled by the specified strategy on the specified output
        void setStrategyMute(routing_strategy strategy,
                             bool on,
                             audio_io_handle_t output,
                             int delayMs = 0,
                             audio_devices_t device = (audio_devices_t)0);

        // Mute or unmute the stream on the specified output
        void setStreamMute(audio_stream_type_t stream,
                           bool on,
                           audio_io_handle_t output,
                           int delayMs = 0,
                           audio_devices_t device = (audio_devices_t)0);

        // handle special cases for sonification strategy while in call: mute streams or replace by
        // a special tone in the device used for communication
        void handleIncallSonification(audio_stream_type_t stream, bool starting, bool stateChange);

        // true if device is in a telephony or VoIP call
        virtual bool isInCall();

        // true if given state represents a device in a telephony or VoIP call
        virtual bool isStateInCall(int state);

        // when a device is connected, checks if an open output can be routed
        // to this device. If none is open, tries to open one of the available outputs.
        // Returns an output suitable to this device or 0.
        // when a device is disconnected, checks if an output is not used any more and
        // returns its handle if any.
        // transfers the audio tracks and effects from one output thread to another accordingly.
        status_t checkOutputsForDevice(const sp<DeviceDescriptor> devDesc,
                                       audio_policy_dev_state_t state,
                                       SortedVector<audio_io_handle_t>& outputs,
                                       const String8 address);

        status_t checkInputsForDevice(audio_devices_t device,
                                      audio_policy_dev_state_t state,
                                      SortedVector<audio_io_handle_t>& inputs,
                                      const String8 address);

        // close an output and its companion duplicating output.
        void closeOutput(audio_io_handle_t output);

        // close an input.
        void closeInput(audio_io_handle_t input);

        // checks and if necessary changes outputs used for all strategies.
        // must be called every time a condition that affects the output choice for a given strategy
        // changes: connected device, phone state, force use...
        // Must be called before updateDevicesAndOutputs()
        void checkOutputForStrategy(routing_strategy strategy);

        // Same as checkOutputForStrategy() but for a all strategies in order of priority
        void checkOutputForAllStrategies();

        // manages A2DP output suspend/restore according to phone state and BT SCO usage
        void checkA2dpSuspend();

        // returns the A2DP output handle if it is open or 0 otherwise
        audio_io_handle_t getA2dpOutput();

        // selects the most appropriate device on output for current state
        // must be called every time a condition that affects the device choice for a given output is
        // changed: connected device, phone state, force use, output start, output stop..
        // see getDeviceForStrategy() for the use of fromCache parameter
        audio_devices_t getNewOutputDevice(audio_io_handle_t output, bool fromCache);

        // updates cache of device used by all strategies (mDeviceForStrategy[])
        // must be called every time a condition that affects the device choice for a given strategy is
        // changed: connected device, phone state, force use...
        // cached values are used by getDeviceForStrategy() if parameter fromCache is true.
         // Must be called after checkOutputForAllStrategies()
        void updateDevicesAndOutputs();

        // selects the most appropriate device on input for current state
        audio_devices_t getNewInputDevice(audio_io_handle_t input);

        virtual uint32_t getMaxEffectsCpuLoad();
        virtual uint32_t getMaxEffectsMemory();
#ifdef AUDIO_POLICY_TEST
        virtual     bool        threadLoop();
                    void        exit();
        int testOutputIndex(audio_io_handle_t output);
#endif //AUDIO_POLICY_TEST

        status_t setEffectEnabled(const sp<EffectDescriptor>& effectDesc, bool enabled);

        // returns the category the device belongs to with regard to volume curve management
        static device_category getDeviceCategory(audio_devices_t device);

        // extract one device relevant for volume control from multiple device selection
        static audio_devices_t getDeviceForVolume(audio_devices_t device);

        SortedVector<audio_io_handle_t> getOutputsForDevice(audio_devices_t device,
                        DefaultKeyedVector<audio_io_handle_t, sp<AudioOutputDescriptor> > openOutputs);
        bool vectorsEqual(SortedVector<audio_io_handle_t>& outputs1,
                                           SortedVector<audio_io_handle_t>& outputs2);

        // mute/unmute strategies using an incompatible device combination
        // if muting, wait for the audio in pcm buffer to be drained before proceeding
        // if unmuting, unmute only after the specified delay
        // Returns the number of ms waited
        uint32_t  checkDeviceMuteStrategies(sp<AudioOutputDescriptor> outputDesc,
                                            audio_devices_t prevDevice,
                                            uint32_t delayMs);

        audio_io_handle_t selectOutput(const SortedVector<audio_io_handle_t>& outputs,
                                       audio_output_flags_t flags);
        // samplingRate parameter is an in/out and so may be modified
        sp<IOProfile> getInputProfile(audio_devices_t device,
                                   uint32_t& samplingRate,
                                   audio_format_t format,
                                   audio_channel_mask_t channelMask,
                                   audio_input_flags_t flags);
        sp<IOProfile> getProfileForDirectOutput(audio_devices_t device,
                                                       uint32_t samplingRate,
                                                       audio_format_t format,
                                                       audio_channel_mask_t channelMask,
                                                       audio_output_flags_t flags);

        audio_io_handle_t selectOutputForEffects(const SortedVector<audio_io_handle_t>& outputs);

        bool isNonOffloadableEffectEnabled();

        status_t addAudioPatch(audio_patch_handle_t handle,
                               const sp<AudioPatch>& patch);
        status_t removeAudioPatch(audio_patch_handle_t handle);

        sp<AudioOutputDescriptor> getOutputFromId(audio_port_handle_t id) const;
        sp<AudioInputDescriptor> getInputFromId(audio_port_handle_t id) const;
        sp<HwModule> getModuleForDevice(audio_devices_t device) const;
        sp<HwModule> getModuleFromName(const char *name) const;
        audio_devices_t availablePrimaryOutputDevices();
        audio_devices_t availablePrimaryInputDevices();

        void updateCallRouting(audio_devices_t rxDevice, int delayMs = 0);

        //
        // Audio policy configuration file parsing (audio_policy.conf)
        //
        static uint32_t stringToEnum(const struct StringToEnum *table,
                                     size_t size,
                                     const char *name);
        static const char *enumToString(const struct StringToEnum *table,
                                      size_t size,
                                      uint32_t value);
        static bool stringToBool(const char *value);
        static audio_output_flags_t parseFlagNames(char *name);
        static audio_devices_t parseDeviceNames(char *name);
        void loadHwModule(cnode *root);
        void loadHwModules(cnode *root);
        void loadGlobalConfig(cnode *root, const sp<HwModule>& module);
        status_t loadAudioPolicyConfig(const char *path);
        void defaultAudioPolicyConfig(void);


        uid_t mUidCached;
        AudioPolicyClientInterface *mpClientInterface;  // audio policy client interface
        audio_io_handle_t mPrimaryOutput;              // primary output handle
        // list of descriptors for outputs currently opened
        DefaultKeyedVector<audio_io_handle_t, sp<AudioOutputDescriptor> > mOutputs;
        // copy of mOutputs before setDeviceConnectionState() opens new outputs
        // reset to mOutputs when updateDevicesAndOutputs() is called.
        DefaultKeyedVector<audio_io_handle_t, sp<AudioOutputDescriptor> > mPreviousOutputs;
        DefaultKeyedVector<audio_io_handle_t, sp<AudioInputDescriptor> > mInputs;     // list of input descriptors
        DeviceVector  mAvailableOutputDevices; // all available output devices
        DeviceVector  mAvailableInputDevices;  // all available input devices
        int mPhoneState;                                                    // current phone state
        audio_policy_forced_cfg_t mForceUse[AUDIO_POLICY_FORCE_USE_CNT];   // current forced use configuration

        StreamDescriptor mStreams[AUDIO_STREAM_CNT];           // stream descriptors for volume control
        bool    mLimitRingtoneVolume;                                       // limit ringtone volume to music volume if headset connected
        audio_devices_t mDeviceForStrategy[NUM_STRATEGIES];
        float   mLastVoiceVolume;                                           // last voice volume value sent to audio HAL

        // Maximum CPU load allocated to audio effects in 0.1 MIPS (ARMv5TE, 0 WS memory) units
        static const uint32_t MAX_EFFECTS_CPU_LOAD = 1000;
        // Maximum memory allocated to audio effects in KB
        static const uint32_t MAX_EFFECTS_MEMORY = 512;
        uint32_t mTotalEffectsCpuLoad; // current CPU load used by effects
        uint32_t mTotalEffectsMemory;  // current memory used by effects
        KeyedVector<int, sp<EffectDescriptor> > mEffects;  // list of registered audio effects
        bool    mA2dpSuspended;  // true if A2DP output is suspended
        sp<DeviceDescriptor> mDefaultOutputDevice; // output device selected by default at boot time
        bool mSpeakerDrcEnabled;// true on devices that use DRC on the DEVICE_CATEGORY_SPEAKER path
                                // to boost soft sounds, used to adjust volume curves accordingly

        Vector < sp<HwModule> > mHwModules;
        volatile int32_t mNextUniqueId;
        volatile int32_t mAudioPortGeneration;

        DefaultKeyedVector<audio_patch_handle_t, sp<AudioPatch> > mAudioPatches;

        DefaultKeyedVector<audio_session_t, audio_io_handle_t> mSoundTriggerSessions;

        sp<AudioPatch> mCallTxPatch;
        sp<AudioPatch> mCallRxPatch;

#ifdef AUDIO_POLICY_TEST
        Mutex   mLock;
        Condition mWaitWorkCV;

        int             mCurOutput;
        bool            mDirectOutput;
        audio_io_handle_t mTestOutputs[NUM_TEST_OUTPUTS];
        int             mTestInput;
        uint32_t        mTestDevice;
        uint32_t        mTestSamplingRate;
        uint32_t        mTestFormat;
        uint32_t        mTestChannels;
        uint32_t        mTestLatencyMs;
#endif //AUDIO_POLICY_TEST

private:
        static float volIndexToAmpl(audio_devices_t device, const StreamDescriptor& streamDesc,
                int indexInUi);
        // updates device caching and output for streams that can influence the
        //    routing of notifications
        void handleNotificationRoutingForStream(audio_stream_type_t stream);
        static bool isVirtualInputDevice(audio_devices_t device);
        static bool deviceDistinguishesOnAddress(audio_devices_t device);
        // find the outputs on a given output descriptor that have the given address.
        // to be called on an AudioOutputDescriptor whose supported devices (as defined
        //   in mProfile->mSupportedDevices) matches the device whose address is to be matched.
        // see deviceDistinguishesOnAddress(audio_devices_t) for whether the device type is one
        //   where addresses are used to distinguish between one connected device and another.
        void findIoHandlesByAddress(sp<AudioOutputDescriptor> desc /*in*/,
                const String8 address /*in*/,
                SortedVector<audio_io_handle_t>& outputs /*out*/);
        uint32_t nextUniqueId();
        uint32_t nextAudioPortGeneration();
        uint32_t curAudioPortGeneration() const { return mAudioPortGeneration; }
        // internal method to return the output handle for the given device and format
        audio_io_handle_t getOutputForDevice(
                audio_devices_t device,
                audio_stream_type_t stream,
                uint32_t samplingRate,
                audio_format_t format,
                audio_channel_mask_t channelMask,
                audio_output_flags_t flags,
                const audio_offload_info_t *offloadInfo);
        // internal function to derive a stream type value from audio attributes
        audio_stream_type_t streamTypefromAttributesInt(const audio_attributes_t *attr);
};

};
