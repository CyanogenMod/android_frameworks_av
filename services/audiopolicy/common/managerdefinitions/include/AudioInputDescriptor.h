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
#include "AudioSession.h"
#include "AudioSessionInfoProvider.h"
#include <utils/Errors.h>
#include <system/audio.h>
#include <utils/SortedVector.h>
#include <utils/KeyedVector.h>

namespace android {

class IOProfile;
class AudioMix;

// descriptor for audio inputs. Used to maintain current configuration of each opened audio input
// and keep track of the usage of this input.
class AudioInputDescriptor: public AudioPortConfig, public AudioSessionInfoProvider
{
public:
    AudioInputDescriptor(const sp<IOProfile>& profile);
    void setIoHandle(audio_io_handle_t ioHandle);
    audio_port_handle_t getId() const;
    audio_module_handle_t getModuleHandle() const;
    uint32_t getOpenRefCount() const;

    status_t    dump(int fd);

    audio_io_handle_t             mIoHandle;       // input handle
    audio_devices_t               mDevice;         // current device this input is routed to
    AudioMix                      *mPolicyMix;     // non NULL when used by a dynamic policy
    const sp<IOProfile>           mProfile;        // I/O profile this output derives from

    virtual void toAudioPortConfig(struct audio_port_config *dstConfig,
            const struct audio_port_config *srcConfig = NULL) const;
    virtual sp<AudioPort> getAudioPort() const { return mProfile; }
    void toAudioPort(struct audio_port *port) const;
    void setPreemptedSessions(const SortedVector<audio_session_t>& sessions);
    SortedVector<audio_session_t> getPreemptedSessions() const;
    bool hasPreemptedSession(audio_session_t session) const;
    void clearPreemptedSessions();
    bool isActive() const;
    bool isSourceActive(audio_source_t source) const;
    audio_source_t inputSource() const;
    bool isSoundTrigger() const;
    status_t addAudioSession(audio_session_t session,
                             const sp<AudioSession>& audioSession);
    status_t removeAudioSession(audio_session_t session);
    sp<AudioSession> getAudioSession(audio_session_t session) const;
    AudioSessionCollection getActiveAudioSessions() const;

    // implementation of AudioSessionInfoProvider
    virtual audio_config_base_t getConfig() const;
    virtual audio_patch_handle_t getPatchHandle() const;

    void setPatchHandle(audio_patch_handle_t handle);

private:
    audio_patch_handle_t          mPatchHandle;
    audio_port_handle_t           mId;
    // audio sessions attached to this input
    AudioSessionCollection        mSessions;
    // Because a preemptible capture session can preempt another one, we end up in an endless loop
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

    // count active capture sessions using one of the specified devices.
    // ignore devices if AUDIO_DEVICE_IN_DEFAULT is passed
    uint32_t activeInputsCountOnDevices(audio_devices_t devices = AUDIO_DEVICE_IN_DEFAULT) const;

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
