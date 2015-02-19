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

#include "ApmImplDefinitions.h"

namespace android {

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
};

}; // namespace android
