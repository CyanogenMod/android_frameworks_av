/*
 * Copyright (C) 2014 The Android Open Source Project
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

// #define LOG_NDEBUG 0
#define LOG_TAG "WebmElement"

#include "EbmlUtil.h"
#include "WebmElement.h"
#include "WebmConstants.h"

#include <media/stagefright/foundation/ADebug.h>
#include <utils/Log.h>

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

using namespace android;
using namespace webm;

namespace {

int64_t voidSize(int64_t totalSize) {
    if (totalSize < 2) {
        return -1;
    }
    if (totalSize < 9) {
        return totalSize - 2;
    }
    return totalSize - 9;
}

uint64_t childrenSum(const List<sp<WebmElement> >& children) {
    uint64_t total = 0;
    for (List<sp<WebmElement> >::const_iterator it = children.begin();
            it != children.end(); ++it) {
        total += (*it)->totalSize();
    }
    return total;
}

void populateCommonTrackEntries(
        int num,
        uint64_t uid,
        bool lacing,
        const char *lang,
        const char *codec,
        TrackTypes type,
        List<sp<WebmElement> > &ls) {
    ls.push_back(new WebmUnsigned(kMkvTrackNumber, num));
    ls.push_back(new WebmUnsigned(kMkvTrackUid, uid));
    ls.push_back(new WebmUnsigned(kMkvFlagLacing, lacing));
    ls.push_back(new WebmString(kMkvLanguage, lang));
    ls.push_back(new WebmString(kMkvCodecId, codec));
    ls.push_back(new WebmUnsigned(kMkvTrackType, type));
}
}

namespace android {

WebmElement::WebmElement(uint64_t id, uint64_t size)
    : mId(id), mSize(size) {
}

WebmElement::~WebmElement() {
}

int WebmElement::serializePayloadSize(uint8_t *buf) {
    return serializeCodedUnsigned(encodeUnsigned(mSize), buf);
}

uint64_t WebmElement::serializeInto(uint8_t *buf) {
    uint8_t *cur = buf;
    int head = serializeCodedUnsigned(mId, cur);
    cur += head;
    int neck = serializePayloadSize(cur);
    cur += neck;
    serializePayload(cur);
    cur += mSize;
    return cur - buf;
}

uint64_t WebmElement::totalSize() {
    uint8_t buf[8];
    //............... + sizeOf(encodeUnsigned(size))
    return sizeOf(mId) + serializePayloadSize(buf) + mSize;
}

uint8_t *WebmElement::serialize(uint64_t& size) {
    size = totalSize();
    uint8_t *buf = new uint8_t[size];
    serializeInto(buf);
    return buf;
}

int WebmElement::write(int fd, uint64_t& size) {
    uint8_t buf[8];
    size = totalSize();
    off64_t off = ::lseek64(fd, (size - 1), SEEK_CUR) - (size - 1);
    ::write(fd, buf, 1); // extend file

    off64_t curOff = off + size;
    off64_t alignedOff = off & ~(::sysconf(_SC_PAGE_SIZE) - 1);
    off64_t mapSize = curOff - alignedOff;
    off64_t pageOff = off - alignedOff;
    void *dst = ::mmap64(NULL, mapSize, PROT_WRITE, MAP_SHARED, fd, alignedOff);
    if ((int) dst == -1) {
        ALOGE("mmap64 failed; errno = %d", errno);
        ALOGE("fd %d; flags: %o", fd, ::fcntl(fd, F_GETFL, 0));
        return errno;
    } else {
        serializeInto((uint8_t*) dst + pageOff);
        ::msync(dst, mapSize, MS_SYNC);
        return ::munmap(dst, mapSize);
    }
}

//=================================================================================================

WebmUnsigned::WebmUnsigned(uint64_t id, uint64_t value)
    : WebmElement(id, sizeOf(value)), mValue(value) {
}

void WebmUnsigned::serializePayload(uint8_t *buf) {
    serializeCodedUnsigned(mValue, buf);
}

//=================================================================================================

WebmFloat::WebmFloat(uint64_t id, double value)
    : WebmElement(id, sizeof(double)), mValue(value) {
}

WebmFloat::WebmFloat(uint64_t id, float value)
    : WebmElement(id, sizeof(float)), mValue(value) {
}

void WebmFloat::serializePayload(uint8_t *buf) {
    uint64_t data;
    if (mSize == sizeof(float)) {
        float f = mValue;
        data = *reinterpret_cast<const uint32_t*>(&f);
    } else {
        data = *reinterpret_cast<const uint64_t*>(&mValue);
    }
    for (int i = mSize - 1; i >= 0; --i) {
        buf[i] = data & 0xff;
        data >>= 8;
    }
}

//=================================================================================================

WebmBinary::WebmBinary(uint64_t id, const sp<ABuffer> &ref)
    : WebmElement(id, ref->size()), mRef(ref) {
}

void WebmBinary::serializePayload(uint8_t *buf) {
    memcpy(buf, mRef->data(), mRef->size());
}

//=================================================================================================

WebmString::WebmString(uint64_t id, const char *str)
    : WebmElement(id, strlen(str)), mStr(str) {
}

void WebmString::serializePayload(uint8_t *buf) {
    memcpy(buf, mStr, strlen(mStr));
}

//=================================================================================================

WebmSimpleBlock::WebmSimpleBlock(
        int trackNum,
        int16_t relTimecode,
        bool key,
        const sp<ABuffer>& orig)
    // ............................ trackNum*1 + timecode*2 + flags*1
    //                                ^^^
    // Only the least significant byte of trackNum is encoded
    : WebmElement(kMkvSimpleBlock, orig->size() + 4),
      mTrackNum(trackNum),
      mRelTimecode(relTimecode),
      mKey(key),
      mRef(orig) {
}

void WebmSimpleBlock::serializePayload(uint8_t *buf) {
    serializeCodedUnsigned(encodeUnsigned(mTrackNum), buf);
    buf[1] = (mRelTimecode & 0xff00) >> 8;
    buf[2] = mRelTimecode & 0xff;
    buf[3] = mKey ? 0x80 : 0;
    memcpy(buf + 4, mRef->data(), mSize - 4);
}

//=================================================================================================

EbmlVoid::EbmlVoid(uint64_t totalSize)
    : WebmElement(kMkvVoid, voidSize(totalSize)),
      mSizeWidth(totalSize - sizeOf(kMkvVoid) - voidSize(totalSize)) {
    CHECK_GE(voidSize(totalSize), 0);
}

int EbmlVoid::serializePayloadSize(uint8_t *buf) {
    return serializeCodedUnsigned(encodeUnsigned(mSize, mSizeWidth), buf);
}

void EbmlVoid::serializePayload(uint8_t *buf) {
    ::memset(buf, 0, mSize);
    return;
}

//=================================================================================================

WebmMaster::WebmMaster(uint64_t id, const List<sp<WebmElement> >& children)
    : WebmElement(id, childrenSum(children)), mChildren(children) {
}

WebmMaster::WebmMaster(uint64_t id)
    : WebmElement(id, 0) {
}

int WebmMaster::serializePayloadSize(uint8_t *buf) {
    if (mSize == 0){
        return serializeCodedUnsigned(kMkvUnknownLength, buf);
    }
    return WebmElement::serializePayloadSize(buf);
}

void WebmMaster::serializePayload(uint8_t *buf) {
    uint64_t off = 0;
    for (List<sp<WebmElement> >::const_iterator it = mChildren.begin(); it != mChildren.end();
            ++it) {
        sp<WebmElement> child = (*it);
        child->serializeInto(buf + off);
        off += child->totalSize();
    }
}

//=================================================================================================

sp<WebmElement> WebmElement::CuePointEntry(uint64_t time, int track, uint64_t off) {
    List<sp<WebmElement> > cuePointEntryFields;
    cuePointEntryFields.push_back(new WebmUnsigned(kMkvCueTrack, track));
    cuePointEntryFields.push_back(new WebmUnsigned(kMkvCueClusterPosition, off));
    WebmElement *cueTrackPositions = new WebmMaster(kMkvCueTrackPositions, cuePointEntryFields);

    cuePointEntryFields.clear();
    cuePointEntryFields.push_back(new WebmUnsigned(kMkvCueTime, time));
    cuePointEntryFields.push_back(cueTrackPositions);
    return new WebmMaster(kMkvCuePoint, cuePointEntryFields);
}

sp<WebmElement> WebmElement::SeekEntry(uint64_t id, uint64_t off) {
    List<sp<WebmElement> > seekEntryFields;
    seekEntryFields.push_back(new WebmUnsigned(kMkvSeekId, id));
    seekEntryFields.push_back(new WebmUnsigned(kMkvSeekPosition, off));
    return new WebmMaster(kMkvSeek, seekEntryFields);
}

sp<WebmElement> WebmElement::EbmlHeader(
        int ver,
        int readVer,
        int maxIdLen,
        int maxSizeLen,
        int docVer,
        int docReadVer) {
    List<sp<WebmElement> > headerFields;
    headerFields.push_back(new WebmUnsigned(kMkvEbmlVersion, ver));
    headerFields.push_back(new WebmUnsigned(kMkvEbmlReadVersion, readVer));
    headerFields.push_back(new WebmUnsigned(kMkvEbmlMaxIdlength, maxIdLen));
    headerFields.push_back(new WebmUnsigned(kMkvEbmlMaxSizeLength, maxSizeLen));
    headerFields.push_back(new WebmString(kMkvDocType, "webm"));
    headerFields.push_back(new WebmUnsigned(kMkvDocTypeVersion, docVer));
    headerFields.push_back(new WebmUnsigned(kMkvDocTypeReadVersion, docReadVer));
    return new WebmMaster(kMkvEbml, headerFields);
}

sp<WebmElement> WebmElement::SegmentInfo(uint64_t scale, double dur) {
    List<sp<WebmElement> > segmentInfo;
    // place duration first; easier to patch
    segmentInfo.push_back(new WebmFloat(kMkvSegmentDuration, dur));
    segmentInfo.push_back(new WebmUnsigned(kMkvTimecodeScale, scale));
    segmentInfo.push_back(new WebmString(kMkvMuxingApp, "android"));
    segmentInfo.push_back(new WebmString(kMkvWritingApp, "android"));
    return new WebmMaster(kMkvInfo, segmentInfo);
}

sp<WebmElement> WebmElement::AudioTrackEntry(
        int chans,
        double rate,
        const sp<ABuffer> &buf,
        int bps,
        uint64_t uid,
        bool lacing,
        const char *lang) {
    if (uid == 0) {
        uid = kAudioTrackNum;
    }

    List<sp<WebmElement> > trackEntryFields;
    populateCommonTrackEntries(
            kAudioTrackNum,
            uid,
            lacing,
            lang,
            "A_VORBIS",
            kAudioType,
            trackEntryFields);

    List<sp<WebmElement> > audioInfo;
    audioInfo.push_back(new WebmUnsigned(kMkvChannels, chans));
    audioInfo.push_back(new WebmFloat(kMkvSamplingFrequency, rate));
    if (bps) {
        WebmElement *bitDepth = new WebmUnsigned(kMkvBitDepth, bps);
        audioInfo.push_back(bitDepth);
    }

    trackEntryFields.push_back(new WebmMaster(kMkvAudio, audioInfo));
    trackEntryFields.push_back(new WebmBinary(kMkvCodecPrivate, buf));
    return new WebmMaster(kMkvTrackEntry, trackEntryFields);
}

sp<WebmElement> WebmElement::VideoTrackEntry(
        uint64_t width,
        uint64_t height,
        uint64_t uid,
        bool lacing,
        const char *lang) {
    if (uid == 0) {
        uid = kVideoTrackNum;
    }

    List<sp<WebmElement> > trackEntryFields;
    populateCommonTrackEntries(
            kVideoTrackNum,
            uid,
            lacing,
            lang,
            "V_VP8",
            kVideoType,
            trackEntryFields);

    List<sp<WebmElement> > videoInfo;
    videoInfo.push_back(new WebmUnsigned(kMkvPixelWidth, width));
    videoInfo.push_back(new WebmUnsigned(kMkvPixelHeight, height));

    trackEntryFields.push_back(new WebmMaster(kMkvVideo, videoInfo));
    return new WebmMaster(kMkvTrackEntry, trackEntryFields);
}
} /* namespace android */
