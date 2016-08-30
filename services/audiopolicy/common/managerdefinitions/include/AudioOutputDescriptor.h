/*
 * Copyright (C) 2015 The Android Open Source Project
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

#pragma once

#include "AudioPort.h"
#include <RoutingStrategy.h>
#include <utils/Errors.h>
#include <utils/Timers.h>
#include <utils/KeyedVector.h>
#include <system/audio.h>
#include "AudioSourceDescriptor.h"

namespace android {

class IOProfile;
class AudioMix;
class AudioPolicyClientInterface;
class DeviceDescriptor;

// descriptor for audio outputs. Used to maintain current configuration of each opened audio output
// and keep track of the usage of this output by each audio stream type.
class AudioOutputDescriptor: public AudioPortConfig
{
public:
    AudioOutputDescriptor(const sp<AudioPort>& port,
                          AudioPolicyClientInterface *clientInterface);
    virtual ~AudioOutputDescriptor() {}

    status_t    dump(int fd);
    void        log(const char* indent);

    audio_port_handle_t getId() const;
    virtual audio_devices_t device() const;
    virtual bool sharesHwModuleWith(const sp<AudioOutputDescriptor> outputDesc);
    virtual audio_devices_t supportedDevices();
    virtual bool isDuplicated() const { return false; }
    virtual uint32_t latency() { return 0; }
    virtual bool isFixedVolume(audio_devices_t device);
    virtual sp<AudioOutputDescriptor> subOutput1() { return 0; }
    virtual sp<AudioOutputDescriptor> subOutput2() { return 0; }
    virtual bool setVolume(float volume,
                           audio_stream_type_t stream,
                           audio_devices_t device,
                           uint32_t delayMs,
                           bool force);
    virtual void changeRefCount(audio_stream_type_t stream, int delta);

    bool isActive(uint32_t inPastMs = 0) const;
    bool isStreamActive(audio_stream_type_t stream,
                        uint32_t inPastMs = 0,
                        nsecs_t sysTime = 0) const;

    virtual void toAudioPortConfig(struct audio_port_config *dstConfig,
                           const struct audio_port_config *srcConfig = NULL) const;
    virtual sp<AudioPort> getAudioPort() const { return mPort; }
    virtual void toAudioPort(struct audio_port *port) const;

    audio_module_handle_t getModuleHandle() const;

    audio_patch_handle_t getPatchHandle() const { return mPatchHandle; };
    void setPatchHandle(audio_patch_handle_t handle) { mPatchHandle = handle; };

    sp<AudioPort>       mPort;
    audio_devices_t mDevice;                   // current device this output is routed to
    audio_io_handle_t mIoHandle;           // output handle
    uint32_t mRefCount[AUDIO_STREAM_CNT]; // number of streams of each type using this output
    nsecs_t mStopTime[AUDIO_STREAM_CNT];
    float mCurVolume[AUDIO_STREAM_CNT];   // current stream volume in dB
    int mMuteCount[AUDIO_STREAM_CNT];     // mute request counter
    bool mStrategyMutedByDevice[NUM_STRATEGIES]; // strategies muted because of incompatible
                                        // device selection. See checkDeviceMuteStrategies()
    AudioPolicyClientInterface *mClientInterface;

protected:
    audio_patch_handle_t mPatchHandle;
    audio_port_handle_t mId;
};

// Audio output driven by a software mixer in audio flinger.
class SwAudioOutputDescriptor: public AudioOutputDescriptor
{
public:
    SwAudioOutputDescriptor(const sp<IOProfile>& profile,
                            AudioPolicyClientInterface *clientInterface);
    virtual ~SwAudioOutputDescriptor() {}

    status_t    dump(int fd);

    void setIoHandle(audio_io_handle_t ioHandle);

    virtual audio_devices_t device() const;
    virtual bool sharesHwModuleWith(const sp<AudioOutputDescriptor> outputDesc);
    virtual audio_devices_t supportedDevices();
    virtual uint32_t latency();
    virtual bool isDuplicated() const { return (mOutput1 != NULL && mOutput2 != NULL); }
    virtual bool isFixedVolume(audio_devices_t device);
    virtual sp<AudioOutputDescriptor> subOutput1() { return mOutput1; }
    virtual sp<AudioOutputDescriptor> subOutput2() { return mOutput2; }
    virtual void changeRefCount(audio_stream_type_t stream, int delta);
    virtual bool setVolume(float volume,
                           audio_stream_type_t stream,
                           audio_devices_t device,
                           uint32_t delayMs,
                           bool force);

    virtual void toAudioPortConfig(struct audio_port_config *dstConfig,
                           const struct audio_port_config *srcConfig = NULL) const;
    virtual void toAudioPort(struct audio_port *port) const;

    const sp<IOProfile> mProfile;          // I/O profile this output derives from
    uint32_t mLatency;                  //
    audio_output_flags_t mFlags;   //
    AudioMix *mPolicyMix;             // non NULL when used by a dynamic policy
    sp<SwAudioOutputDescriptor> mOutput1;    // used by duplicated outputs: first output
    sp<SwAudioOutputDescriptor> mOutput2;    // used by duplicated outputs: second output
    uint32_t mDirectOpenCount; // number of clients using this output (direct outputs only)
    uint32_t mGlobalRefCount;  // non-stream-specific ref count
};

// Audio output driven by an input device directly.
class HwAudioOutputDescriptor: public AudioOutputDescriptor
{
public:
    HwAudioOutputDescriptor(const sp<AudioSourceDescriptor>& source,
                            AudioPolicyClientInterface *clientInterface);
    virtual ~HwAudioOutputDescriptor() {}

    status_t    dump(int fd);

    virtual audio_devices_t supportedDevices();
    virtual bool setVolume(float volume,
                           audio_stream_type_t stream,
                           audio_devices_t device,
                           uint32_t delayMs,
                           bool force);

    virtual void toAudioPortConfig(struct audio_port_config *dstConfig,
                           const struct audio_port_config *srcConfig = NULL) const;
    virtual void toAudioPort(struct audio_port *port) const;

    const sp<AudioSourceDescriptor> mSource;

};

class SwAudioOutputCollection :
        public DefaultKeyedVector< audio_io_handle_t, sp<SwAudioOutputDescriptor> >
{
public:
    bool isStreamActive(audio_stream_type_t stream, uint32_t inPastMs = 0) const;

    /**
     * return whether a stream is playing remotely, override to change the definition of
     * local/remote playback, used for instance by notification manager to not make
     * media players lose audio focus when not playing locally
     * For the base implementation, "remotely" means playing during screen mirroring which
     * uses an output for playback with a non-empty, non "0" address.
     */
    bool isStreamActiveRemotely(audio_stream_type_t stream, uint32_t inPastMs = 0) const;

    /**
     * returns the A2DP output handle if it is open or 0 otherwise
     */
    audio_io_handle_t getA2dpOutput() const;

    /**
     * return true if primary HAL supports A2DP Playback
     */
    bool isA2dpOnPrimary() const;

    sp<SwAudioOutputDescriptor> getOutputFromId(audio_port_handle_t id) const;

    sp<SwAudioOutputDescriptor> getPrimaryOutput() const;

    /**
     * return true if any output is playing anything besides the stream to ignore
     */
    bool isAnyOutputActive(audio_stream_type_t streamToIgnore) const;

    audio_devices_t getSupportedDevices(audio_io_handle_t handle) const;

    status_t dump(int fd) const;
};

class HwAudioOutputCollection :
        public DefaultKeyedVector< audio_io_handle_t, sp<HwAudioOutputDescriptor> >
{
public:
    bool isStreamActive(audio_stream_type_t stream, uint32_t inPastMs = 0) const;

    /**
     * return true if any output is playing anything besides the stream to ignore
     */
    bool isAnyOutputActive(audio_stream_type_t streamToIgnore) const;

    status_t dump(int fd) const;
};


}; // namespace android
