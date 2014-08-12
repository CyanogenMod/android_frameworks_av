/*
 * Copyright 2014, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaCodecInfo"
#include <utils/Log.h>

#include <media/IOMX.h>

#include <media/MediaCodecInfo.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <binder/Parcel.h>

#include <media/stagefright/OMXCodec.h>

namespace android {

void MediaCodecInfo::Capabilities::getSupportedProfileLevels(
        Vector<ProfileLevel> *profileLevels) const {
    profileLevels->clear();
    profileLevels->appendVector(mProfileLevels);
}

void MediaCodecInfo::Capabilities::getSupportedColorFormats(
        Vector<uint32_t> *colorFormats) const {
    colorFormats->clear();
    colorFormats->appendVector(mColorFormats);
}

uint32_t MediaCodecInfo::Capabilities::getFlags() const {
    return mFlags;
}

const sp<AMessage> MediaCodecInfo::Capabilities::getDetails() const {
    return mDetails;
}

MediaCodecInfo::Capabilities::Capabilities()
  : mFlags(0) {
    mDetails = new AMessage;
}

// static
sp<MediaCodecInfo::Capabilities> MediaCodecInfo::Capabilities::FromParcel(
        const Parcel &parcel) {
    sp<MediaCodecInfo::Capabilities> caps = new Capabilities();
    size_t size = static_cast<size_t>(parcel.readInt32());
    for (size_t i = 0; i < size; i++) {
        ProfileLevel profileLevel;
        profileLevel.mProfile = static_cast<uint32_t>(parcel.readInt32());
        profileLevel.mLevel = static_cast<uint32_t>(parcel.readInt32());
        if (caps != NULL) {
            caps->mProfileLevels.push_back(profileLevel);
        }
    }
    size = static_cast<size_t>(parcel.readInt32());
    for (size_t i = 0; i < size; i++) {
        uint32_t color = static_cast<uint32_t>(parcel.readInt32());
        if (caps != NULL) {
            caps->mColorFormats.push_back(color);
        }
    }
    uint32_t flags = static_cast<uint32_t>(parcel.readInt32());
    sp<AMessage> details = AMessage::FromParcel(parcel);
    if (caps != NULL) {
        caps->mFlags = flags;
        caps->mDetails = details;
    }
    return caps;
}

status_t MediaCodecInfo::Capabilities::writeToParcel(Parcel *parcel) const {
    CHECK_LE(mProfileLevels.size(), INT32_MAX);
    parcel->writeInt32(mProfileLevels.size());
    for (size_t i = 0; i < mProfileLevels.size(); i++) {
        parcel->writeInt32(mProfileLevels.itemAt(i).mProfile);
        parcel->writeInt32(mProfileLevels.itemAt(i).mLevel);
    }
    CHECK_LE(mColorFormats.size(), INT32_MAX);
    parcel->writeInt32(mColorFormats.size());
    for (size_t i = 0; i < mColorFormats.size(); i++) {
        parcel->writeInt32(mColorFormats.itemAt(i));
    }
    parcel->writeInt32(mFlags);
    mDetails->writeToParcel(parcel);
    return OK;
}

bool MediaCodecInfo::isEncoder() const {
    return mIsEncoder;
}

bool MediaCodecInfo::hasQuirk(const char *name) const {
    for (size_t ix = 0; ix < mQuirks.size(); ix++) {
        if (mQuirks.itemAt(ix).equalsIgnoreCase(name)) {
            return true;
        }
    }
    return false;
}

void MediaCodecInfo::getSupportedMimes(Vector<AString> *mimes) const {
    mimes->clear();
    for (size_t ix = 0; ix < mCaps.size(); ix++) {
        mimes->push_back(mCaps.keyAt(ix));
    }
}

const sp<MediaCodecInfo::Capabilities>
MediaCodecInfo::getCapabilitiesFor(const char *mime) const {
    ssize_t ix = getCapabilityIndex(mime);
    if (ix >= 0) {
        return mCaps.valueAt(ix);
    }
    return NULL;
}

const char *MediaCodecInfo::getCodecName() const {
    return mName.c_str();
}

// static
sp<MediaCodecInfo> MediaCodecInfo::FromParcel(const Parcel &parcel) {
    AString name = AString::FromParcel(parcel);
    bool isEncoder = static_cast<bool>(parcel.readInt32());
    sp<MediaCodecInfo> info = new MediaCodecInfo(name, isEncoder, NULL);
    size_t size = static_cast<size_t>(parcel.readInt32());
    for (size_t i = 0; i < size; i++) {
        AString quirk = AString::FromParcel(parcel);
        if (info != NULL) {
            info->mQuirks.push_back(quirk);
        }
    }
    size = static_cast<size_t>(parcel.readInt32());
    for (size_t i = 0; i < size; i++) {
        AString mime = AString::FromParcel(parcel);
        sp<Capabilities> caps = Capabilities::FromParcel(parcel);
        if (info != NULL) {
            info->mCaps.add(mime, caps);
        }
    }
    return info;
}

status_t MediaCodecInfo::writeToParcel(Parcel *parcel) const {
    mName.writeToParcel(parcel);
    parcel->writeInt32(mIsEncoder);
    parcel->writeInt32(mQuirks.size());
    for (size_t i = 0; i < mQuirks.size(); i++) {
        mQuirks.itemAt(i).writeToParcel(parcel);
    }
    parcel->writeInt32(mCaps.size());
    for (size_t i = 0; i < mCaps.size(); i++) {
        mCaps.keyAt(i).writeToParcel(parcel);
        mCaps.valueAt(i)->writeToParcel(parcel);
    }
    return OK;
}

ssize_t MediaCodecInfo::getCapabilityIndex(const char *mime) const {
    for (size_t ix = 0; ix < mCaps.size(); ix++) {
        if (mCaps.keyAt(ix).equalsIgnoreCase(mime)) {
            return ix;
        }
    }
    return -1;
}

MediaCodecInfo::MediaCodecInfo(AString name, bool encoder, const char *mime)
    : mName(name),
      mIsEncoder(encoder),
      mHasSoleMime(false) {
    if (mime != NULL) {
        addMime(mime);
        mHasSoleMime = true;
    }
}

status_t MediaCodecInfo::addMime(const char *mime) {
    if (mHasSoleMime) {
        ALOGE("Codec '%s' already had its type specified", mName.c_str());
        return -EINVAL;
    }
    ssize_t ix = getCapabilityIndex(mime);
    if (ix >= 0) {
        mCurrentCaps = mCaps.valueAt(ix);
    } else {
        mCurrentCaps = new Capabilities();
        mCaps.add(AString(mime), mCurrentCaps);
    }
    return OK;
}

status_t MediaCodecInfo::initializeCapabilities(const CodecCapabilities &caps) {
    mCurrentCaps->mProfileLevels.clear();
    mCurrentCaps->mColorFormats.clear();

    for (size_t i = 0; i < caps.mProfileLevels.size(); ++i) {
        const CodecProfileLevel &src = caps.mProfileLevels.itemAt(i);

        ProfileLevel profileLevel;
        profileLevel.mProfile = src.mProfile;
        profileLevel.mLevel = src.mLevel;
        mCurrentCaps->mProfileLevels.push_back(profileLevel);
    }

    for (size_t i = 0; i < caps.mColorFormats.size(); ++i) {
        mCurrentCaps->mColorFormats.push_back(caps.mColorFormats.itemAt(i));
    }

    mCurrentCaps->mFlags = caps.mFlags;
    mCurrentCaps->mDetails = new AMessage;

    return OK;
}

void MediaCodecInfo::addQuirk(const char *name) {
    if (!hasQuirk(name)) {
        mQuirks.push(name);
    }
}

void MediaCodecInfo::complete() {
    mCurrentCaps = NULL;
}

void MediaCodecInfo::addDetail(const AString &key, const AString &value) {
    mCurrentCaps->mDetails->setString(key.c_str(), value.c_str());
}

void MediaCodecInfo::addFeature(const AString &key, int32_t value) {
    AString tag = "feature-";
    tag.append(key);
    mCurrentCaps->mDetails->setInt32(tag.c_str(), value);
}

}  // namespace android
