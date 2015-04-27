/*
 * Copyright 2015 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaResource"
#include <utils/Log.h>
#include <media/MediaResource.h>

namespace android {

const char kResourceSecureCodec[] = "secure-codec";
const char kResourceNonSecureCodec[] = "non-secure-codec";
const char kResourceAudioCodec[] = "audio-codec";
const char kResourceVideoCodec[] = "video-codec";
const char kResourceGraphicMemory[] = "graphic-memory";

MediaResource::MediaResource() : mValue(0) {}

MediaResource::MediaResource(String8 type, uint64_t value)
        : mType(type),
          mValue(value) {}

MediaResource::MediaResource(String8 type, String8 subType, uint64_t value)
        : mType(type),
          mSubType(subType),
          mValue(value) {}

void MediaResource::readFromParcel(const Parcel &parcel) {
    mType = parcel.readString8();
    mSubType = parcel.readString8();
    mValue = parcel.readUint64();
}

void MediaResource::writeToParcel(Parcel *parcel) const {
    parcel->writeString8(mType);
    parcel->writeString8(mSubType);
    parcel->writeUint64(mValue);
}

String8 MediaResource::toString() const {
    String8 str;
    str.appendFormat("%s/%s:%llu", mType.string(), mSubType.string(), (unsigned long long)mValue);
    return str;
}

bool MediaResource::operator==(const MediaResource &other) const {
    return (other.mType == mType) && (other.mSubType == mSubType) && (other.mValue == mValue);
}

bool MediaResource::operator!=(const MediaResource &other) const {
    return !(*this == other);
}

}; // namespace android
