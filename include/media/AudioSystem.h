/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 * Not a Contribution.
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

#ifndef ANDROID_AUDIOSYSTEM_H_
#define ANDROID_AUDIOSYSTEM_H_

#include <utils/RefBase.h>
#include <utils/threads.h>
#include <media/IAudioFlinger.h>

#include <system/audio.h>
#include <system/audio_policy.h>

/* XXX: Should be include by all the users instead */
#include <media/AudioParameter.h>

namespace android {

typedef void (*audio_error_callback)(status_t err);

class IAudioPolicyService;
class String8;

class AudioSystem
{
public:

    /* These are static methods to control the system-wide AudioFlinger
     * only privileged processes can have access to them
     */

    // mute/unmute microphone
    static status_t muteMicrophone(bool state);
    static status_t isMicrophoneMuted(bool *state);

    // set/get master volume
    static status_t setMasterVolume(float value);
    static status_t getMasterVolume(float* volume);

    // mute/unmute audio outputs
    static status_t setMasterMute(bool mute);
    static status_t getMasterMute(bool* mute);

    // set/get stream volume on specified output
    static status_t setStreamVolume(audio_stream_type_t stream, float value,
                                    audio_io_handle_t output);
    static status_t getStreamVolume(audio_stream_type_t stream, float* volume,
                                    audio_io_handle_t output);

    // mute/unmute stream
    static status_t setStreamMute(audio_stream_type_t stream, bool mute);
    static status_t getStreamMute(audio_stream_type_t stream, bool* mute);

    // set audio mode in audio hardware
    static status_t setMode(audio_mode_t mode);

    // returns true in *state if tracks are active on the specified stream or have been active
    // in the past inPastMs milliseconds
    static status_t isStreamActive(audio_stream_type_t stream, bool *state, uint32_t inPastMs = 0);
    // returns true in *state if tracks are active for what qualifies as remote playback
    // on the specified stream or have been active in the past inPastMs milliseconds. Remote
    // playback isn't mutually exclusive with local playback.
    static status_t isStreamActiveRemotely(audio_stream_type_t stream, bool *state,
            uint32_t inPastMs = 0);
    // returns true in *state if a recorder is currently recording with the specified source
    static status_t isSourceActive(audio_source_t source, bool *state);

    // set/get audio hardware parameters. The function accepts a list of parameters
    // key value pairs in the form: key1=value1;key2=value2;...
    // Some keys are reserved for standard parameters (See AudioParameter class).
    static status_t setParameters(audio_io_handle_t ioHandle, const String8& keyValuePairs);
    static String8  getParameters(audio_io_handle_t ioHandle, const String8& keys);

    static void setErrorCallback(audio_error_callback cb);

    // helper function to obtain AudioFlinger service handle
    static const sp<IAudioFlinger>& get_audio_flinger();

    static float linearToLog(int volume);
    static int logToLinear(float volume);

    static status_t getOutputSamplingRate(uint32_t* samplingRate,
            audio_stream_type_t stream = AUDIO_STREAM_DEFAULT);
    static status_t getOutputFrameCount(size_t* frameCount,
            audio_stream_type_t stream = AUDIO_STREAM_DEFAULT);
    static status_t getOutputLatency(uint32_t* latency,
            audio_stream_type_t stream = AUDIO_STREAM_DEFAULT);
    static status_t getSamplingRate(audio_io_handle_t output,
                                          audio_stream_type_t streamType,
                                          uint32_t* samplingRate);
    // returns the number of frames per audio HAL write buffer. Corresponds to
    // audio_stream->get_buffer_size()/audio_stream_frame_size()
    static status_t getFrameCount(audio_io_handle_t output,
                                  audio_stream_type_t stream,
                                  size_t* frameCount);
    // returns the audio output stream latency in ms. Corresponds to
    // audio_stream_out->get_latency()
    static status_t getLatency(audio_io_handle_t output,
                               audio_stream_type_t stream,
                               uint32_t* latency);

    static bool routedToA2dpOutput(audio_stream_type_t streamType);

    static status_t getInputBufferSize(uint32_t sampleRate, audio_format_t format,
        audio_channel_mask_t channelMask, size_t* buffSize);

    static status_t setVoiceVolume(float volume);

    // return the number of audio frames written by AudioFlinger to audio HAL and
    // audio dsp to DAC since the output on which the specified stream is playing
    // has exited standby.
    // returned status (from utils/Errors.h) can be:
    // - NO_ERROR: successful operation, halFrames and dspFrames point to valid data
    // - INVALID_OPERATION: Not supported on current hardware platform
    // - BAD_VALUE: invalid parameter
    // NOTE: this feature is not supported on all hardware platforms and it is
    // necessary to check returned status before using the returned values.
    static status_t getRenderPosition(size_t *halFrames, size_t *dspFrames,
            audio_stream_type_t stream = AUDIO_STREAM_DEFAULT);

    // return the number of input frames lost by HAL implementation, or 0 if the handle is invalid
    static size_t getInputFramesLost(audio_io_handle_t ioHandle);

    static int newAudioSessionId();
    static void acquireAudioSessionId(int audioSession);
    static void releaseAudioSessionId(int audioSession);

    // types of io configuration change events received with ioConfigChanged()
    enum io_config_event {
        OUTPUT_OPENED,
        OUTPUT_CLOSED,
        OUTPUT_CONFIG_CHANGED,
        INPUT_OPENED,
        INPUT_CLOSED,
        INPUT_CONFIG_CHANGED,
        STREAM_CONFIG_CHANGED,
#ifdef QCOM_HARDWARE
        EFFECT_CONFIG_CHANGED,
#endif
        NUM_CONFIG_EVENTS
    };

    // audio output descriptor used to cache output configurations in client process to avoid
    // frequent calls through IAudioFlinger
    class OutputDescriptor {
    public:
        OutputDescriptor()
        : samplingRate(0), format(AUDIO_FORMAT_DEFAULT), channels(0), frameCount(0), latency(0)  {}

        uint32_t samplingRate;
        int32_t format;
        int32_t channels;
        size_t frameCount;
        uint32_t latency;
    };

    // Events used to synchronize actions between audio sessions.
    // For instance SYNC_EVENT_PRESENTATION_COMPLETE can be used to delay recording start until
    // playback is complete on another audio session.
    // See definitions in MediaSyncEvent.java
    enum sync_event_t {
        SYNC_EVENT_SAME = -1,             // used internally to indicate restart with same event
        SYNC_EVENT_NONE = 0,
        SYNC_EVENT_PRESENTATION_COMPLETE,

        //
        // Define new events here: SYNC_EVENT_START, SYNC_EVENT_STOP, SYNC_EVENT_TIME ...
        //
        SYNC_EVENT_CNT,
    };

    // Timeout for synchronous record start. Prevents from blocking the record thread forever
    // if the trigger event is not fired.
    static const uint32_t kSyncRecordStartTimeOutMs = 30000;

    //
    // IAudioPolicyService interface (see AudioPolicyInterface for method descriptions)
    //
    static status_t setDeviceConnectionState(audio_devices_t device, audio_policy_dev_state_t state,
                                                const char *device_address);
    static audio_policy_dev_state_t getDeviceConnectionState(audio_devices_t device,
                                                                const char *device_address);
    static status_t setPhoneState(audio_mode_t state);
    static status_t setForceUse(audio_policy_force_use_t usage, audio_policy_forced_cfg_t config);
    static audio_policy_forced_cfg_t getForceUse(audio_policy_force_use_t usage);
    static audio_io_handle_t getOutput(audio_stream_type_t stream,
                                        uint32_t samplingRate = 0,
                                        audio_format_t format = AUDIO_FORMAT_DEFAULT,
                                        audio_channel_mask_t channelMask = AUDIO_CHANNEL_OUT_STEREO,
                                        audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE);
    static status_t startOutput(audio_io_handle_t output,
                                audio_stream_type_t stream,
                                int session = 0);
    static status_t stopOutput(audio_io_handle_t output,
                               audio_stream_type_t stream,
                               int session = 0);
    static void releaseOutput(audio_io_handle_t output);
    static audio_io_handle_t getInput(audio_source_t inputSource,
                                    uint32_t samplingRate = 0,
                                    audio_format_t format = AUDIO_FORMAT_DEFAULT,
                                    audio_channel_mask_t channelMask = AUDIO_CHANNEL_IN_MONO,
                                    int sessionId = 0);
    static status_t startInput(audio_io_handle_t input);
    static status_t stopInput(audio_io_handle_t input);
    static void releaseInput(audio_io_handle_t input);
    static status_t initStreamVolume(audio_stream_type_t stream,
                                      int indexMin,
                                      int indexMax);
    static status_t setStreamVolumeIndex(audio_stream_type_t stream,
                                         int index,
                                         audio_devices_t device);
    static status_t getStreamVolumeIndex(audio_stream_type_t stream,
                                         int *index,
                                         audio_devices_t device);

    static uint32_t getStrategyForStream(audio_stream_type_t stream);
    static audio_devices_t getDevicesForStream(audio_stream_type_t stream);

    static audio_io_handle_t getOutputForEffect(const effect_descriptor_t *desc);
    static status_t registerEffect(const effect_descriptor_t *desc,
                                    audio_io_handle_t io,
                                    uint32_t strategy,
                                    int session,
                                    int id);
    static status_t unregisterEffect(int id);
    static status_t setEffectEnabled(int id, bool enabled);

    // clear stream to output mapping cache (gStreamOutputMap)
    // and output configuration cache (gOutputs)
    static void clearAudioConfigCache();

    static const sp<IAudioPolicyService>& get_audio_policy_service();

    // helpers for android.media.AudioManager.getProperty(), see description there for meaning
    static uint32_t getPrimaryOutputSamplingRate();
    static size_t getPrimaryOutputFrameCount();

    // ----------------------------------------------------------------------------

private:

    class AudioFlingerClient: public IBinder::DeathRecipient, public BnAudioFlingerClient
    {
    public:
        AudioFlingerClient() {
        }

        // DeathRecipient
        virtual void binderDied(const wp<IBinder>& who);

        // IAudioFlingerClient

        // indicate a change in the configuration of an output or input: keeps the cached
        // values for output/input parameters up-to-date in client process
        virtual void ioConfigChanged(int event, audio_io_handle_t ioHandle, const void *param2);
    };

    class AudioPolicyServiceClient: public IBinder::DeathRecipient
    {
    public:
        AudioPolicyServiceClient() {
        }

        // DeathRecipient
        virtual void binderDied(const wp<IBinder>& who);
    };

    static sp<AudioFlingerClient> gAudioFlingerClient;
    static sp<AudioPolicyServiceClient> gAudioPolicyServiceClient;
    friend class AudioFlingerClient;
    friend class AudioPolicyServiceClient;

    static Mutex gLock;
    static sp<IAudioFlinger> gAudioFlinger;
    static audio_error_callback gAudioErrorCallback;

    static size_t gInBuffSize;
    // previous parameters for recording buffer size queries
    static uint32_t gPrevInSamplingRate;
    static audio_format_t gPrevInFormat;
    static audio_channel_mask_t gPrevInChannelMask;

    static sp<IAudioPolicyService> gAudioPolicyService;

    // mapping between stream types and outputs
    static DefaultKeyedVector<audio_stream_type_t, audio_io_handle_t> gStreamOutputMap;
    // list of output descriptors containing cached parameters
    // (sampling rate, framecount, channel count...)
    static DefaultKeyedVector<audio_io_handle_t, OutputDescriptor *> gOutputs;
};

};  // namespace android

#endif  /*ANDROID_AUDIOSYSTEM_H_*/
