/*
 * Copyright (C) 2016 The CyanogenMod Project
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

#ifndef ANDROID_AUDIOSESSION_H
#define ANDROID_AUDIOSESSION_H

#include <stdint.h>
#include <sys/types.h>

#include <system/audio.h>

#include <utils/RefBase.h>
#include <utils/Errors.h>


namespace android {

// class to store streaminfo
class AudioSessionInfo : public RefBase {
public:
    AudioSessionInfo(int session, audio_stream_type_t stream, audio_output_flags_t flags,
            audio_channel_mask_t channelMask, uid_t uid) :
        mSessionId(session), mStream(stream), mFlags(flags), mChannelMask(channelMask),
        mUid(uid), mRefCount(0) {}

    /*virtual*/ ~AudioSessionInfo() {}

    const int mSessionId;
    const audio_stream_type_t mStream;
    audio_output_flags_t mFlags;
    audio_channel_mask_t mChannelMask;
    const uid_t mUid;

    // AudioPolicyManager keeps mLock, no need for lock on reference count here
    int mRefCount;
};


}; // namespace android

#endif // ANDROID_AUDIOSESSION_H
