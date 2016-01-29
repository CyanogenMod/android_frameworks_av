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

#include <system/audio.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <media/AudioPolicy.h>

namespace android {

class AudioPolicyClientInterface;

class AudioSession : public RefBase
{
public:
    AudioSession(audio_session_t session,
                 audio_source_t inputSource,
                 audio_format_t format,
                 uint32_t sampleRate,
                 audio_channel_mask_t channelMask,
                 audio_input_flags_t flags,
                 uid_t uid,
                 bool isSoundTrigger,
                 AudioMix* policyMix,
                 AudioPolicyClientInterface *clientInterface);

    status_t dump(int fd, int spaces, int index) const;

    audio_session_t session() const { return mSession; }
    audio_source_t inputSource()const { return mInputSource; }
    audio_format_t format() const { return mFormat; }
    uint32_t sampleRate() const { return mSampleRate; }
    audio_channel_mask_t channelMask() const { return mChannelMask; }
    audio_input_flags_t flags() const { return mFlags; }
    uid_t uid() const { return mUid; }
    bool matches(const sp<AudioSession> &other) const;
    bool isSoundTrigger() const { return mIsSoundTrigger; }
    uint32_t openCount() const { return mOpenCount; } ;
    uint32_t activeCount() const { return mActiveCount; } ;

    uint32_t changeOpenCount(int delta);
    uint32_t changeActiveCount(int delta);

private:
    const audio_session_t mSession;
    const audio_source_t mInputSource;
    const audio_format_t mFormat;
    const uint32_t mSampleRate;
    const audio_channel_mask_t mChannelMask;
    const audio_input_flags_t mFlags;
    const uid_t mUid;
    bool  mIsSoundTrigger;
    uint32_t  mOpenCount;
    uint32_t  mActiveCount;
    AudioMix* mPolicyMix; // non NULL when used by a dynamic policy
    AudioPolicyClientInterface* mClientInterface;
};

class AudioSessionCollection :
    public DefaultKeyedVector<audio_session_t, sp<AudioSession> >
{
public:
    status_t addSession(audio_session_t session,
                             const sp<AudioSession>& audioSession);

    status_t removeSession(audio_session_t session);

    uint32_t getOpenCount() const;

    AudioSessionCollection getActiveSessions() const;
    bool hasActiveSession() const;
    bool isSourceActive(audio_source_t source) const;
    audio_source_t getHighestPrioritySource(bool activeOnly) const;

    status_t dump(int fd, int spaces) const;
};

}; // namespace android
