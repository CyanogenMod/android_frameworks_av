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
#include <binder/Parcel.h>

namespace android {

// class to store streaminfo
class AudioSessionInfo : public RefBase {
public:
    AudioSessionInfo(audio_session_t session, audio_stream_type_t stream, audio_output_flags_t flags,
            audio_channel_mask_t channelMask, uid_t uid) :
        mSessionId(session), mStream(stream), mFlags(flags), mChannelMask(channelMask),
        mUid(uid), mRefCount(0) {}

    AudioSessionInfo() : mSessionId((audio_session_t) 0), mStream(AUDIO_STREAM_DEFAULT), mFlags(AUDIO_OUTPUT_FLAG_NONE), mChannelMask(AUDIO_CHANNEL_NONE), mUid(0) {}

    /*virtual*/ ~AudioSessionInfo() {}

    audio_session_t mSessionId;
    audio_stream_type_t mStream;
    audio_output_flags_t mFlags;
    audio_channel_mask_t mChannelMask;
    uid_t mUid;

    // AudioPolicyManager keeps mLock, no need for lock on reference count here
    int mRefCount;

    void readFromParcel(const Parcel &parcel)  {
        mSessionId = (audio_session_t) parcel.readInt32();
        mStream = static_cast<audio_stream_type_t>(parcel.readInt32());
        mFlags = static_cast<audio_output_flags_t>(parcel.readInt32());
        mChannelMask = static_cast<audio_channel_mask_t>(parcel.readInt32());
        mUid = static_cast<uid_t>(parcel.readInt32());
    }

    void writeToParcel(Parcel *parcel) const {
        parcel->writeInt32(mSessionId);
        parcel->writeInt32(mStream);
        parcel->writeInt32(mFlags);
        parcel->writeInt32(mChannelMask);
        parcel->writeInt32(mUid);
    }
};

}; // namespace android

#endif // ANDROID_AUDIOSESSION_H
