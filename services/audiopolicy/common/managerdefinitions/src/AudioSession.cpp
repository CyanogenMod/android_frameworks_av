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

#define LOG_TAG "APM::AudioSession"
//#define LOG_NDEBUG 0

#include <AudioPolicyInterface.h>
#include "AudioSession.h"
#include "AudioGain.h"
#include "TypeConverter.h"
#include <cutils/log.h>
#include <utils/String8.h>

namespace android {

AudioSession::AudioSession(audio_session_t session,
                           audio_source_t inputSource,
                           audio_format_t format,
                           uint32_t sampleRate,
                           audio_channel_mask_t channelMask,
                           audio_input_flags_t flags,
                           uid_t uid,
                           bool isSoundTrigger,
                           AudioMix* policyMix,
                           AudioPolicyClientInterface *clientInterface) :
    mSession(session), mInputSource(inputSource),
    mFormat(format), mSampleRate(sampleRate), mChannelMask(channelMask),
    mFlags(flags), mUid(uid), mIsSoundTrigger(isSoundTrigger),
    mOpenCount(1), mActiveCount(0), mPolicyMix(policyMix), mClientInterface(clientInterface)
{
}

uint32_t AudioSession::changeOpenCount(int delta)
{
    if ((delta + (int)mOpenCount) < 0) {
        ALOGW("%s invalid delta %d, open count %d",
              __FUNCTION__, delta, mOpenCount);
        mOpenCount = (uint32_t)(-delta);
    }
    mOpenCount += delta;
    ALOGV("%s open count %d", __FUNCTION__, mOpenCount);
    return mOpenCount;
}

uint32_t AudioSession::changeActiveCount(int delta)
{
    const uint32_t oldActiveCount = mActiveCount;
    if ((delta + (int)mActiveCount) < 0) {
        ALOGW("%s invalid delta %d, active count %d",
              __FUNCTION__, delta, mActiveCount);
        mActiveCount = (uint32_t)(-delta);
    }
    mActiveCount += delta;
    ALOGV("%s active count %d", __FUNCTION__, mActiveCount);

    if ((oldActiveCount == 0) && (mActiveCount > 0)) {
        // if input maps to a dynamic policy with an activity listener, notify of state change
        if ((mPolicyMix != NULL) && ((mPolicyMix->mCbFlags & AudioMix::kCbFlagNotifyActivity) != 0))
        {
            mClientInterface->onDynamicPolicyMixStateUpdate(mPolicyMix->mRegistrationId,
                    MIX_STATE_MIXING);
        }
        mClientInterface->onRecordingConfigurationUpdate(RECORD_CONFIG_EVENT_START,
                mSession, mInputSource);
    } else if ((oldActiveCount > 0) && (mActiveCount == 0)) {
        // if input maps to a dynamic policy with an activity listener, notify of state change
        if ((mPolicyMix != NULL) && ((mPolicyMix->mCbFlags & AudioMix::kCbFlagNotifyActivity) != 0))
        {
            mClientInterface->onDynamicPolicyMixStateUpdate(mPolicyMix->mRegistrationId,
                    MIX_STATE_IDLE);
        }
        mClientInterface->onRecordingConfigurationUpdate(RECORD_CONFIG_EVENT_STOP,
                mSession, mInputSource);
    }

    return mActiveCount;
}

bool AudioSession::matches(const sp<AudioSession> &other) const
{
    if (other->session() == mSession &&
        other->inputSource() == mInputSource &&
        other->format() == mFormat &&
        other->sampleRate() == mSampleRate &&
        other->channelMask() == mChannelMask &&
        other->flags() == mFlags &&
        other->uid() == mUid) {
        return true;
    }
    return false;
}


status_t AudioSession::dump(int fd, int spaces, int index) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "%*sAudio session %d:\n", spaces, "", index+1);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- session: %2d\n", spaces, "", mSession);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- owner uid: %2d\n", spaces, "", mUid);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- input source: %d\n", spaces, "", mInputSource);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- format: %08x\n", spaces, "", mFormat);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- sample: %d\n", spaces, "", mSampleRate);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- channel mask: %08x\n",
             spaces, "", mChannelMask);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- is soundtrigger: %s\n",
             spaces, "", mIsSoundTrigger ? "true" : "false");
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- open count: %d\n", spaces, "", mOpenCount);
    result.append(buffer);
    snprintf(buffer, SIZE, "%*s- active count: %d\n", spaces, "", mActiveCount);
    result.append(buffer);

    write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioSessionCollection::addSession(audio_session_t session,
                                         const sp<AudioSession>& audioSession)
{
    ssize_t index = indexOfKey(session);

    if (index >= 0) {
        ALOGW("addSession() session %d already in", session);
        return ALREADY_EXISTS;
    }
    add(session, audioSession);
    ALOGV("addSession() session %d  client %d source %d",
            session, audioSession->uid(), audioSession->inputSource());
    return NO_ERROR;
}

status_t AudioSessionCollection::removeSession(audio_session_t session)
{
    ssize_t index = indexOfKey(session);

    if (index < 0) {
        ALOGW("removeSession() session %d not in", session);
        return ALREADY_EXISTS;
    }
    ALOGV("removeSession() session %d", session);
    removeItemsAt(index);
    return NO_ERROR;
}

uint32_t AudioSessionCollection::getOpenCount() const
{
    uint32_t openCount = 0;
    for (size_t i = 0; i < size(); i++) {
        openCount += valueAt(i)->openCount();
    }
    return openCount;
}

AudioSessionCollection AudioSessionCollection::getActiveSessions() const
{
    AudioSessionCollection activeSessions;
    for (size_t i = 0; i < size(); i++) {
        if (valueAt(i)->activeCount() != 0) {
            activeSessions.add(valueAt(i)->session(), valueAt(i));
        }
    }
    return activeSessions;
}

bool AudioSessionCollection::hasActiveSession() const
{
    return getActiveSessions().size() != 0;
}

bool AudioSessionCollection::isSourceActive(audio_source_t source) const
{
    for (size_t i = 0; i < size(); i++) {
        const sp<AudioSession>  audioSession = valueAt(i);
        // AUDIO_SOURCE_HOTWORD is equivalent to AUDIO_SOURCE_VOICE_RECOGNITION only if it
        // corresponds to an active capture triggered by a hardware hotword recognition
        if (audioSession->activeCount() > 0 &&
                ((audioSession->inputSource() == source) ||
                ((source == AUDIO_SOURCE_VOICE_RECOGNITION) &&
                 (audioSession->inputSource() == AUDIO_SOURCE_HOTWORD) &&
                 audioSession->isSoundTrigger()))) {
            return true;
        }
    }
    return false;
}


status_t AudioSessionCollection::dump(int fd, int spaces) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    snprintf(buffer, SIZE, "%*sAudio Sessions:\n", spaces, "");
    write(fd, buffer, strlen(buffer));
    for (size_t i = 0; i < size(); i++) {
        valueAt(i)->dump(fd, spaces + 2, i);
    }
    return NO_ERROR;
}

}; // namespace android
