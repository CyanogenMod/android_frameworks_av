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

namespace android {

class IOProfile;
class AudioMix;

// descriptor for audio outputs. Used to maintain current configuration of each opened audio output
// and keep track of the usage of this output by each audio stream type.
class AudioOutputDescriptor: public AudioPortConfig
{
public:
    AudioOutputDescriptor(const sp<IOProfile>& profile);

    status_t    dump(int fd);
    void        log(const char* indent);

    audio_devices_t device() const;
    void changeRefCount(audio_stream_type_t stream, int delta);
    audio_port_handle_t getId() const;
    void setIoHandle(audio_io_handle_t ioHandle);
    bool isDuplicated() const { return (mOutput1 != NULL && mOutput2 != NULL); }
    audio_devices_t supportedDevices();
    uint32_t latency();
    bool sharesHwModuleWith(const sp<AudioOutputDescriptor> outputDesc);
    bool isActive(uint32_t inPastMs = 0) const;
    bool isStreamActive(audio_stream_type_t stream,
                        uint32_t inPastMs = 0,
                        nsecs_t sysTime = 0) const;

    virtual void toAudioPortConfig(struct audio_port_config *dstConfig,
                           const struct audio_port_config *srcConfig = NULL) const;
    virtual sp<AudioPort> getAudioPort() const { return mProfile; }
    void toAudioPort(struct audio_port *port) const;

    audio_module_handle_t getModuleHandle() const;

    audio_io_handle_t mIoHandle;              // output handle
    uint32_t mLatency;                  //
    audio_output_flags_t mFlags;   //
    audio_devices_t mDevice;                   // current device this output is routed to
    AudioMix *mPolicyMix;             // non NULL when used by a dynamic policy
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

private:
    audio_port_handle_t mId;
};

class AudioOutputCollection :
        public DefaultKeyedVector< audio_io_handle_t, sp<AudioOutputDescriptor> >
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

    sp<AudioOutputDescriptor> getOutputFromId(audio_port_handle_t id) const;

    sp<AudioOutputDescriptor> getPrimaryOutput() const;

    /**
     * return true if any output is playing anything besides the stream to ignore
     */
    bool isAnyOutputActive(audio_stream_type_t streamToIgnore) const;

    audio_devices_t getSupportedDevices(audio_io_handle_t handle) const;

    status_t dump(int fd) const;
};

}; // namespace android
