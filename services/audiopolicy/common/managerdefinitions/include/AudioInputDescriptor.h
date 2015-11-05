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
#include <utils/Errors.h>
#include <system/audio.h>
#include <utils/SortedVector.h>
#include <utils/KeyedVector.h>

namespace android {

class IOProfile;
class AudioMix;

// descriptor for audio inputs. Used to maintain current configuration of each opened audio input
// and keep track of the usage of this input.
class AudioInputDescriptor: public AudioPortConfig
{
public:
    AudioInputDescriptor(const sp<IOProfile>& profile);
    void setIoHandle(audio_io_handle_t ioHandle);
    audio_port_handle_t getId() const;
    audio_module_handle_t getModuleHandle() const;

    status_t    dump(int fd);

    audio_io_handle_t             mIoHandle;       // input handle
    audio_devices_t               mDevice;         // current device this input is routed to
    AudioMix                      *mPolicyMix;     // non NULL when used by a dynamic policy
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
    void setPreemptedSessions(const SortedVector<audio_session_t>& sessions);
    SortedVector<audio_session_t> getPreemptedSessions() const;
    bool hasPreemptedSession(audio_session_t session) const;
    void clearPreemptedSessions();

private:
    audio_port_handle_t           mId;
    // Because a preemtible capture session can preempt another one, we end up in an endless loop
    // situation were each session is allowed to restart after being preempted,
    // thus preempting the other one which restarts and so on.
    // To avoid this situation, we store which audio session was preempted when
    // a particular input started and prevent preemption of this active input by this session.
    // We also inherit sessions from the preempted input to avoid a 3 way preemption loop etc...
    SortedVector<audio_session_t> mPreemptedSessions;

};

class AudioInputCollection :
        public DefaultKeyedVector< audio_io_handle_t, sp<AudioInputDescriptor> >
{
public:
    bool isSourceActive(audio_source_t source) const;

    sp<AudioInputDescriptor> getInputFromId(audio_port_handle_t id) const;

    uint32_t activeInputsCount() const;

    /**
     * return io handle of active input or 0 if no input is active
     * Only considers inputs from physical devices (e.g. main mic, headset mic) when
     * ignoreVirtualInputs is true.
     */
    audio_io_handle_t getActiveInput(bool ignoreVirtualInputs = true);

    audio_devices_t getSupportedDevices(audio_io_handle_t handle) const;

    status_t dump(int fd) const;
};


}; // namespace android
