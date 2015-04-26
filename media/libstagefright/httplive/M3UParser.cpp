/*
 * Copyright (C) 2010 The Android Open Source Project
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
#define LOG_TAG "M3UParser"
#include <utils/Log.h>

#include "M3UParser.h"
#include <binder/Parcel.h>
#include <cutils/properties.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>
#include <media/mediaplayer.h>

namespace android {

struct M3UParser::MediaGroup : public RefBase {
    enum Type {
        TYPE_AUDIO,
        TYPE_VIDEO,
        TYPE_SUBS,
        TYPE_CC,
    };

    enum FlagBits {
        FLAG_AUTOSELECT         = 1,
        FLAG_DEFAULT            = 2,
        FLAG_FORCED             = 4,
        FLAG_HAS_LANGUAGE       = 8,
        FLAG_HAS_URI            = 16,
    };

    MediaGroup(Type type);

    Type type() const;

    status_t addMedia(
            const char *name,
            const char *uri,
            const char *language,
            uint32_t flags);

    bool getActiveURI(AString *uri) const;

    void pickRandomMediaItems();
    status_t selectTrack(size_t index, bool select);
    size_t countTracks() const;
    sp<AMessage> getTrackInfo(size_t index) const;

protected:
    virtual ~MediaGroup();

private:

    friend struct M3UParser;

    struct Media {
        AString mName;
        AString mURI;
        AString mLanguage;
        uint32_t mFlags;
    };

    Type mType;
    Vector<Media> mMediaItems;

    ssize_t mSelectedIndex;

    DISALLOW_EVIL_CONSTRUCTORS(MediaGroup);
};

M3UParser::MediaGroup::MediaGroup(Type type)
    : mType(type),
      mSelectedIndex(-1) {
}

M3UParser::MediaGroup::~MediaGroup() {
}

M3UParser::MediaGroup::Type M3UParser::MediaGroup::type() const {
    return mType;
}

status_t M3UParser::MediaGroup::addMedia(
        const char *name,
        const char *uri,
        const char *language,
        uint32_t flags) {
    mMediaItems.push();
    Media &item = mMediaItems.editItemAt(mMediaItems.size() - 1);

    item.mName = name;

    if (uri) {
        item.mURI = uri;
    }

    if (language) {
        item.mLanguage = language;
    }

    item.mFlags = flags;

    return OK;
}

void M3UParser::MediaGroup::pickRandomMediaItems() {
#if 1
    switch (mType) {
        case TYPE_AUDIO:
        {
            char value[PROPERTY_VALUE_MAX];
            if (property_get("media.httplive.audio-index", value, NULL)) {
                char *end;
                mSelectedIndex = strtoul(value, &end, 10);
                CHECK(end > value && *end == '\0');

                if (mSelectedIndex >= (ssize_t)mMediaItems.size()) {
                    mSelectedIndex = mMediaItems.size() - 1;
                }
            } else {
                mSelectedIndex = 0;
            }
            break;
        }

        case TYPE_VIDEO:
        {
            mSelectedIndex = 0;
            break;
        }

        case TYPE_SUBS:
        {
            mSelectedIndex = -1;
            break;
        }

        default:
            TRESPASS();
    }
#else
    mSelectedIndex = (rand() * mMediaItems.size()) / RAND_MAX;
#endif
}

status_t M3UParser::MediaGroup::selectTrack(size_t index, bool select) {
    if (mType != TYPE_SUBS && mType != TYPE_AUDIO) {
        ALOGE("only select subtitile/audio tracks for now!");
        return INVALID_OPERATION;
    }

    if (select) {
        if (index >= mMediaItems.size()) {
            ALOGE("track %zu does not exist", index);
            return INVALID_OPERATION;
        }
        if (mSelectedIndex == (ssize_t)index) {
            ALOGE("track %zu already selected", index);
            return BAD_VALUE;
        }
        ALOGV("selected track %zu", index);
        mSelectedIndex = index;
    } else {
        if (mSelectedIndex != (ssize_t)index) {
            ALOGE("track %zu is not selected", index);
            return BAD_VALUE;
        }
        ALOGV("unselected track %zu", index);
        mSelectedIndex = -1;
    }

    return OK;
}

size_t M3UParser::MediaGroup::countTracks() const {
    return mMediaItems.size();
}

sp<AMessage> M3UParser::MediaGroup::getTrackInfo(size_t index) const {
    if (index >= mMediaItems.size()) {
        return NULL;
    }

    sp<AMessage> format = new AMessage();

    int32_t trackType;
    if (mType == TYPE_AUDIO) {
        trackType = MEDIA_TRACK_TYPE_AUDIO;
    } else if (mType == TYPE_VIDEO) {
        trackType = MEDIA_TRACK_TYPE_VIDEO;
    } else if (mType == TYPE_SUBS) {
        trackType = MEDIA_TRACK_TYPE_SUBTITLE;
    } else {
        trackType = MEDIA_TRACK_TYPE_UNKNOWN;
    }
    format->setInt32("type", trackType);

    const Media &item = mMediaItems.itemAt(index);
    const char *lang = item.mLanguage.empty() ? "und" : item.mLanguage.c_str();
    format->setString("language", lang);

    if (mType == TYPE_SUBS) {
        // TO-DO: pass in a MediaFormat instead
        format->setString("mime", MEDIA_MIMETYPE_TEXT_VTT);
        format->setInt32("auto", !!(item.mFlags & MediaGroup::FLAG_AUTOSELECT));
        format->setInt32("default", !!(item.mFlags & MediaGroup::FLAG_DEFAULT));
        format->setInt32("forced", !!(item.mFlags & MediaGroup::FLAG_FORCED));
    }

    return format;
}

bool M3UParser::MediaGroup::getActiveURI(AString *uri) const {
    for (size_t i = 0; i < mMediaItems.size(); ++i) {
        if (mSelectedIndex >= 0 && i == (size_t)mSelectedIndex) {
            const Media &item = mMediaItems.itemAt(i);

            *uri = item.mURI;
            return true;
        }
    }

    return false;
}

////////////////////////////////////////////////////////////////////////////////

M3UParser::M3UParser(
        const char *baseURI, const void *data, size_t size)
    : mInitCheck(NO_INIT),
      mBaseURI(baseURI),
      mIsExtM3U(false),
      mIsVariantPlaylist(false),
      mIsComplete(false),
      mIsEvent(false),
      mDiscontinuitySeq(0),
      mSelectedIndex(-1) {
    mInitCheck = parse(data, size);
}

M3UParser::~M3UParser() {
}

status_t M3UParser::initCheck() const {
    return mInitCheck;
}

bool M3UParser::isExtM3U() const {
    return mIsExtM3U;
}

bool M3UParser::isVariantPlaylist() const {
    return mIsVariantPlaylist;
}

bool M3UParser::isComplete() const {
    return mIsComplete;
}

bool M3UParser::isEvent() const {
    return mIsEvent;
}

size_t M3UParser::getDiscontinuitySeq() const {
    return mDiscontinuitySeq;
}

sp<AMessage> M3UParser::meta() {
    return mMeta;
}

size_t M3UParser::size() {
    return mItems.size();
}

bool M3UParser::itemAt(size_t index, AString *uri, sp<AMessage> *meta) {
    if (uri) {
        uri->clear();
    }

    if (meta) {
        *meta = NULL;
    }

    if (index >= mItems.size()) {
        return false;
    }

    if (uri) {
        *uri = mItems.itemAt(index).mURI;
    }

    if (meta) {
        *meta = mItems.itemAt(index).mMeta;
    }

    return true;
}

void M3UParser::pickRandomMediaItems() {
    for (size_t i = 0; i < mMediaGroups.size(); ++i) {
        mMediaGroups.valueAt(i)->pickRandomMediaItems();
    }
}

status_t M3UParser::selectTrack(size_t index, bool select) {
    for (size_t i = 0, ii = index; i < mMediaGroups.size(); ++i) {
        sp<MediaGroup> group = mMediaGroups.valueAt(i);
        size_t tracks = group->countTracks();
        if (ii < tracks) {
            status_t err = group->selectTrack(ii, select);
            if (err == OK) {
                mSelectedIndex = select ? index : -1;
            }
            return err;
        }
        ii -= tracks;
    }
    return INVALID_OPERATION;
}

size_t M3UParser::getTrackCount() const {
    size_t trackCount = 0;
    for (size_t i = 0; i < mMediaGroups.size(); ++i) {
        trackCount += mMediaGroups.valueAt(i)->countTracks();
    }
    return trackCount;
}

sp<AMessage> M3UParser::getTrackInfo(size_t index) const {
    for (size_t i = 0, ii = index; i < mMediaGroups.size(); ++i) {
        sp<MediaGroup> group = mMediaGroups.valueAt(i);
        size_t tracks = group->countTracks();
        if (ii < tracks) {
            return group->getTrackInfo(ii);
        }
        ii -= tracks;
    }
    return NULL;
}

ssize_t M3UParser::getSelectedIndex() const {
    return mSelectedIndex;
}

ssize_t M3UParser::getSelectedTrack(media_track_type type) const {
    MediaGroup::Type groupType;
    switch (type) {
        case MEDIA_TRACK_TYPE_VIDEO:
            groupType = MediaGroup::TYPE_VIDEO;
            break;

        case MEDIA_TRACK_TYPE_AUDIO:
            groupType = MediaGroup::TYPE_AUDIO;
            break;

        case MEDIA_TRACK_TYPE_SUBTITLE:
            groupType = MediaGroup::TYPE_SUBS;
            break;

        default:
            return -1;
    }

    for (size_t i = 0, ii = 0; i < mMediaGroups.size(); ++i) {
        sp<MediaGroup> group = mMediaGroups.valueAt(i);
        size_t tracks = group->countTracks();
        if (groupType != group->mType) {
            ii += tracks;
        } else if (group->mSelectedIndex >= 0) {
            return ii + group->mSelectedIndex;
        }
    }

    return -1;
}

bool M3UParser::getTypeURI(size_t index, const char *key, AString *uri) const {
    if (!mIsVariantPlaylist) {
        *uri = mBaseURI;

        // Assume media without any more specific attribute contains
        // audio and video, but no subtitles.
        return !strcmp("audio", key) || !strcmp("video", key);
    }

    CHECK_LT(index, mItems.size());

    sp<AMessage> meta = mItems.itemAt(index).mMeta;

    AString groupID;
    if (!meta->findString(key, &groupID)) {
        *uri = mItems.itemAt(index).mURI;

        AString codecs;
        if (!meta->findString("codecs", &codecs)) {
            // Assume media without any more specific attribute contains
            // audio and video, but no subtitles.
            return !strcmp("audio", key) || !strcmp("video", key);
        } else {
            // Split the comma separated list of codecs.
            size_t offset = 0;
            ssize_t commaPos = -1;
            codecs.append(',');
            while ((commaPos = codecs.find(",", offset)) >= 0) {
                AString codec(codecs, offset, commaPos - offset);
                codec.trim();
                // return true only if a codec of type `key` ("audio"/"video")
                // is found.
                if (codecIsType(codec, key)) {
                    return true;
                }
                offset = commaPos + 1;
            }
            return false;
        }
    }

    sp<MediaGroup> group = mMediaGroups.valueFor(groupID);
    if (!group->getActiveURI(uri)) {
        return false;
    }

    if ((*uri).empty()) {
        *uri = mItems.itemAt(index).mURI;
    }

    return true;
}

static bool MakeURL(const char *baseURL, const char *url, AString *out) {
    out->clear();

    if (strncasecmp("http://", baseURL, 7)
            && strncasecmp("https://", baseURL, 8)
            && strncasecmp("file://", baseURL, 7)) {
        // Base URL must be absolute
        return false;
    }
    const size_t schemeEnd = (strstr(baseURL, "//") - baseURL) + 2;
    CHECK(schemeEnd == 7 || schemeEnd == 8);

    if (!strncasecmp("http://", url, 7) || !strncasecmp("https://", url, 8)) {
        // "url" is already an absolute URL, ignore base URL.
        out->setTo(url);

        ALOGV("base:'%s', url:'%s' => '%s'", baseURL, url, out->c_str());

        return true;
    }

    if (url[0] == '/') {
        // URL is an absolute path.

        char *protocolEnd = strstr(baseURL, "//") + 2;
        char *pathStart = strchr(protocolEnd, '/');

        if (pathStart != NULL) {
            out->setTo(baseURL, pathStart - baseURL);
        } else {
            out->setTo(baseURL);
        }

        out->append(url);
    } else {
        // URL is a relative path

        // Check for a possible query string
        const char *qsPos = strchr(baseURL, '?');
        size_t end;
        if (qsPos != NULL) {
            end = qsPos - baseURL;
        } else {
            end = strlen(baseURL);
        }
        // Check for the last slash before a potential query string
        for (ssize_t pos = end - 1; pos >= 0; pos--) {
            if (baseURL[pos] == '/') {
                end = pos;
                break;
            }
        }

        // Check whether the found slash actually is part of the path
        // and not part of the "http://".
        if (end >= schemeEnd) {
            out->setTo(baseURL, end);
        } else {
            out->setTo(baseURL);
        }

        out->append("/");
        out->append(url);
    }

    ALOGV("base:'%s', url:'%s' => '%s'", baseURL, url, out->c_str());

    return true;
}

status_t M3UParser::parse(const void *_data, size_t size) {
    int32_t lineNo = 0;

    sp<AMessage> itemMeta;

    const char *data = (const char *)_data;
    size_t offset = 0;
    uint64_t segmentRangeOffset = 0;
    while (offset < size) {
        size_t offsetLF = offset;
        while (offsetLF < size && data[offsetLF] != '\n') {
            ++offsetLF;
        }

        AString line;
        if (offsetLF > offset && data[offsetLF - 1] == '\r') {
            line.setTo(&data[offset], offsetLF - offset - 1);
        } else {
            line.setTo(&data[offset], offsetLF - offset);
        }

        // ALOGI("#%s#", line.c_str());

        if (line.empty()) {
            offset = offsetLF + 1;
            continue;
        }

        if (lineNo == 0 && line == "#EXTM3U") {
            mIsExtM3U = true;
        }

        if (mIsExtM3U) {
            status_t err = OK;

            if (line.startsWith("#EXT-X-TARGETDURATION")) {
                if (mIsVariantPlaylist) {
                    return ERROR_MALFORMED;
                }
                err = parseMetaData(line, &mMeta, "target-duration");
            } else if (line.startsWith("#EXT-X-MEDIA-SEQUENCE")) {
                if (mIsVariantPlaylist) {
                    return ERROR_MALFORMED;
                }
                err = parseMetaData(line, &mMeta, "media-sequence");
            } else if (line.startsWith("#EXT-X-KEY")) {
                if (mIsVariantPlaylist) {
                    return ERROR_MALFORMED;
                }
                err = parseCipherInfo(line, &itemMeta, mBaseURI);
            } else if (line.startsWith("#EXT-X-ENDLIST")) {
                mIsComplete = true;
            } else if (line.startsWith("#EXT-X-PLAYLIST-TYPE:EVENT")) {
                mIsEvent = true;
            } else if (line.startsWith("#EXTINF")) {
                if (mIsVariantPlaylist) {
                    return ERROR_MALFORMED;
                }
                err = parseMetaDataDuration(line, &itemMeta, "durationUs");
            } else if (line.startsWith("#EXT-X-DISCONTINUITY")) {
                if (mIsVariantPlaylist) {
                    return ERROR_MALFORMED;
                }
                if (itemMeta == NULL) {
                    itemMeta = new AMessage;
                }
                itemMeta->setInt32("discontinuity", true);
            } else if (line.startsWith("#EXT-X-STREAM-INF")) {
                if (mMeta != NULL) {
                    return ERROR_MALFORMED;
                }
                mIsVariantPlaylist = true;
                err = parseStreamInf(line, &itemMeta);
            } else if (line.startsWith("#EXT-X-BYTERANGE")) {
                if (mIsVariantPlaylist) {
                    return ERROR_MALFORMED;
                }

                uint64_t length, offset;
                err = parseByteRange(line, segmentRangeOffset, &length, &offset);

                if (err == OK) {
                    if (itemMeta == NULL) {
                        itemMeta = new AMessage;
                    }

                    itemMeta->setInt64("range-offset", offset);
                    itemMeta->setInt64("range-length", length);

                    segmentRangeOffset = offset + length;
                }
            } else if (line.startsWith("#EXT-X-MEDIA")) {
                err = parseMedia(line);
            } else if (line.startsWith("#EXT-X-DISCONTINUITY-SEQUENCE")) {
                size_t seq;
                err = parseDiscontinuitySequence(line, &seq);
                if (err == OK) {
                    mDiscontinuitySeq = seq;
                }
            }

            if (err != OK) {
                return err;
            }
        }

        if (!line.startsWith("#")) {
            if (!mIsVariantPlaylist) {
                int64_t durationUs;
                if (itemMeta == NULL
                        || !itemMeta->findInt64("durationUs", &durationUs)) {
                    return ERROR_MALFORMED;
                }
            }

            mItems.push();
            Item *item = &mItems.editItemAt(mItems.size() - 1);

            CHECK(MakeURL(mBaseURI.c_str(), line.c_str(), &item->mURI));

            item->mMeta = itemMeta;

            itemMeta.clear();
        }

        offset = offsetLF + 1;
        ++lineNo;
    }

    return OK;
}

// static
status_t M3UParser::parseMetaData(
        const AString &line, sp<AMessage> *meta, const char *key) {
    ssize_t colonPos = line.find(":");

    if (colonPos < 0) {
        return ERROR_MALFORMED;
    }

    int32_t x;
    status_t err = ParseInt32(line.c_str() + colonPos + 1, &x);

    if (err != OK) {
        return err;
    }

    if (meta->get() == NULL) {
        *meta = new AMessage;
    }
    (*meta)->setInt32(key, x);

    return OK;
}

// static
status_t M3UParser::parseMetaDataDuration(
        const AString &line, sp<AMessage> *meta, const char *key) {
    ssize_t colonPos = line.find(":");

    if (colonPos < 0) {
        return ERROR_MALFORMED;
    }

    double x;
    status_t err = ParseDouble(line.c_str() + colonPos + 1, &x);

    if (err != OK) {
        return err;
    }

    if (meta->get() == NULL) {
        *meta = new AMessage;
    }
    (*meta)->setInt64(key, (int64_t)(x * 1E6));

    return OK;
}

// Find the next occurence of the character "what" at or after "offset",
// but ignore occurences between quotation marks.
// Return the index of the occurrence or -1 if not found.
static ssize_t FindNextUnquoted(
        const AString &line, char what, size_t offset) {
    CHECK_NE((int)what, (int)'"');

    bool quoted = false;
    while (offset < line.size()) {
        char c = line.c_str()[offset];

        if (c == '"') {
            quoted = !quoted;
        } else if (c == what && !quoted) {
            return offset;
        }

        ++offset;
    }

    return -1;
}

status_t M3UParser::parseStreamInf(
        const AString &line, sp<AMessage> *meta) const {
    ssize_t colonPos = line.find(":");

    if (colonPos < 0) {
        return ERROR_MALFORMED;
    }

    size_t offset = colonPos + 1;

    while (offset < line.size()) {
        ssize_t end = FindNextUnquoted(line, ',', offset);
        if (end < 0) {
            end = line.size();
        }

        AString attr(line, offset, end - offset);
        attr.trim();

        offset = end + 1;

        ssize_t equalPos = attr.find("=");
        if (equalPos < 0) {
            continue;
        }

        AString key(attr, 0, equalPos);
        key.trim();

        AString val(attr, equalPos + 1, attr.size() - equalPos - 1);
        val.trim();

        ALOGV("key=%s value=%s", key.c_str(), val.c_str());

        if (!strcasecmp("bandwidth", key.c_str())) {
            const char *s = val.c_str();
            char *end;
            unsigned long x = strtoul(s, &end, 10);

            if (end == s || *end != '\0') {
                // malformed
                continue;
            }

            if (meta->get() == NULL) {
                *meta = new AMessage;
            }
            (*meta)->setInt32("bandwidth", x);
        } else if (!strcasecmp("codecs", key.c_str())) {
            if (!isQuotedString(val)) {
                ALOGE("Expected quoted string for %s attribute, "
                      "got '%s' instead.",
                      key.c_str(), val.c_str());;

                return ERROR_MALFORMED;
            }

            key.tolower();
            const AString &codecs = unquoteString(val);
            if (meta->get() == NULL) {
                *meta = new AMessage;
            }
            (*meta)->setString(key.c_str(), codecs.c_str());
        } else if (!strcasecmp("audio", key.c_str())
                || !strcasecmp("video", key.c_str())
                || !strcasecmp("subtitles", key.c_str())) {
            if (!isQuotedString(val)) {
                ALOGE("Expected quoted string for %s attribute, "
                      "got '%s' instead.",
                      key.c_str(), val.c_str());

                return ERROR_MALFORMED;
            }

            const AString &groupID = unquoteString(val);
            ssize_t groupIndex = mMediaGroups.indexOfKey(groupID);

            if (groupIndex < 0) {
                ALOGE("Undefined media group '%s' referenced in stream info.",
                      groupID.c_str());

                return ERROR_MALFORMED;
            }

            key.tolower();
            if (meta->get() == NULL) {
                *meta = new AMessage;
            }
            (*meta)->setString(key.c_str(), groupID.c_str());
        }
    }

    return OK;
}

// static
status_t M3UParser::parseCipherInfo(
        const AString &line, sp<AMessage> *meta, const AString &baseURI) {
    ssize_t colonPos = line.find(":");

    if (colonPos < 0) {
        return ERROR_MALFORMED;
    }

    size_t offset = colonPos + 1;

    while (offset < line.size()) {
        ssize_t end = FindNextUnquoted(line, ',', offset);
        if (end < 0) {
            end = line.size();
        }

        AString attr(line, offset, end - offset);
        attr.trim();

        offset = end + 1;

        ssize_t equalPos = attr.find("=");
        if (equalPos < 0) {
            continue;
        }

        AString key(attr, 0, equalPos);
        key.trim();

        AString val(attr, equalPos + 1, attr.size() - equalPos - 1);
        val.trim();

        ALOGV("key=%s value=%s", key.c_str(), val.c_str());

        key.tolower();

        if (key == "method" || key == "uri" || key == "iv") {
            if (meta->get() == NULL) {
                *meta = new AMessage;
            }

            if (key == "uri") {
                if (val.size() >= 2
                        && val.c_str()[0] == '"'
                        && val.c_str()[val.size() - 1] == '"') {
                    // Remove surrounding quotes.
                    AString tmp(val, 1, val.size() - 2);
                    val = tmp;
                }

                AString absURI;
                if (MakeURL(baseURI.c_str(), val.c_str(), &absURI)) {
                    val = absURI;
                } else {
                    ALOGE("failed to make absolute url for %s.",
                            uriDebugString(baseURI).c_str());
                }
            }

            key.insert(AString("cipher-"), 0);

            (*meta)->setString(key.c_str(), val.c_str(), val.size());
        }
    }

    return OK;
}

// static
status_t M3UParser::parseByteRange(
        const AString &line, uint64_t curOffset,
        uint64_t *length, uint64_t *offset) {
    ssize_t colonPos = line.find(":");

    if (colonPos < 0) {
        return ERROR_MALFORMED;
    }

    ssize_t atPos = line.find("@", colonPos + 1);

    AString lenStr;
    if (atPos < 0) {
        lenStr = AString(line, colonPos + 1, line.size() - colonPos - 1);
    } else {
        lenStr = AString(line, colonPos + 1, atPos - colonPos - 1);
    }

    lenStr.trim();

    const char *s = lenStr.c_str();
    char *end;
    *length = strtoull(s, &end, 10);

    if (s == end || *end != '\0') {
        return ERROR_MALFORMED;
    }

    if (atPos >= 0) {
        AString offStr = AString(line, atPos + 1, line.size() - atPos - 1);
        offStr.trim();

        const char *s = offStr.c_str();
        *offset = strtoull(s, &end, 10);

        if (s == end || *end != '\0') {
            return ERROR_MALFORMED;
        }
    } else {
        *offset = curOffset;
    }

    return OK;
}

status_t M3UParser::parseMedia(const AString &line) {
    ssize_t colonPos = line.find(":");

    if (colonPos < 0) {
        return ERROR_MALFORMED;
    }

    bool haveGroupType = false;
    MediaGroup::Type groupType = MediaGroup::TYPE_AUDIO;

    bool haveGroupID = false;
    AString groupID;

    bool haveGroupLanguage = false;
    AString groupLanguage;

    bool haveGroupName = false;
    AString groupName;

    bool haveGroupAutoselect = false;
    bool groupAutoselect = false;

    bool haveGroupDefault = false;
    bool groupDefault = false;

    bool haveGroupForced = false;
    bool groupForced = false;

    bool haveGroupURI = false;
    AString groupURI;

    size_t offset = colonPos + 1;

    while (offset < line.size()) {
        ssize_t end = FindNextUnquoted(line, ',', offset);
        if (end < 0) {
            end = line.size();
        }

        AString attr(line, offset, end - offset);
        attr.trim();

        offset = end + 1;

        ssize_t equalPos = attr.find("=");
        if (equalPos < 0) {
            continue;
        }

        AString key(attr, 0, equalPos);
        key.trim();

        AString val(attr, equalPos + 1, attr.size() - equalPos - 1);
        val.trim();

        ALOGV("key=%s value=%s", key.c_str(), val.c_str());

        if (!strcasecmp("type", key.c_str())) {
            if (!strcasecmp("subtitles", val.c_str())) {
                groupType = MediaGroup::TYPE_SUBS;
            } else if (!strcasecmp("audio", val.c_str())) {
                groupType = MediaGroup::TYPE_AUDIO;
            } else if (!strcasecmp("video", val.c_str())) {
                groupType = MediaGroup::TYPE_VIDEO;
            } else if (!strcasecmp("closed-captions", val.c_str())){
                groupType = MediaGroup::TYPE_CC;
            } else {
                ALOGE("Invalid media group type '%s'", val.c_str());
                return ERROR_MALFORMED;
            }

            haveGroupType = true;
        } else if (!strcasecmp("group-id", key.c_str())) {
            if (val.size() < 2
                    || val.c_str()[0] != '"'
                    || val.c_str()[val.size() - 1] != '"') {
                ALOGE("Expected quoted string for GROUP-ID, got '%s' instead.",
                      val.c_str());

                return ERROR_MALFORMED;
            }

            groupID.setTo(val, 1, val.size() - 2);
            haveGroupID = true;
        } else if (!strcasecmp("language", key.c_str())) {
            if (val.size() < 2
                    || val.c_str()[0] != '"'
                    || val.c_str()[val.size() - 1] != '"') {
                ALOGE("Expected quoted string for LANGUAGE, got '%s' instead.",
                      val.c_str());

                return ERROR_MALFORMED;
            }

            groupLanguage.setTo(val, 1, val.size() - 2);
            haveGroupLanguage = true;
        } else if (!strcasecmp("name", key.c_str())) {
            if (val.size() < 2
                    || val.c_str()[0] != '"'
                    || val.c_str()[val.size() - 1] != '"') {
                ALOGE("Expected quoted string for NAME, got '%s' instead.",
                      val.c_str());

                return ERROR_MALFORMED;
            }

            groupName.setTo(val, 1, val.size() - 2);
            haveGroupName = true;
        } else if (!strcasecmp("autoselect", key.c_str())) {
            groupAutoselect = false;
            if (!strcasecmp("YES", val.c_str())) {
                groupAutoselect = true;
            } else if (!strcasecmp("NO", val.c_str())) {
                groupAutoselect = false;
            } else {
                ALOGE("Expected YES or NO for AUTOSELECT attribute, "
                      "got '%s' instead.",
                      val.c_str());

                return ERROR_MALFORMED;
            }

            haveGroupAutoselect = true;
        } else if (!strcasecmp("default", key.c_str())) {
            groupDefault = false;
            if (!strcasecmp("YES", val.c_str())) {
                groupDefault = true;
            } else if (!strcasecmp("NO", val.c_str())) {
                groupDefault = false;
            } else {
                ALOGE("Expected YES or NO for DEFAULT attribute, "
                      "got '%s' instead.",
                      val.c_str());

                return ERROR_MALFORMED;
            }

            haveGroupDefault = true;
        } else if (!strcasecmp("forced", key.c_str())) {
            groupForced = false;
            if (!strcasecmp("YES", val.c_str())) {
                groupForced = true;
            } else if (!strcasecmp("NO", val.c_str())) {
                groupForced = false;
            } else {
                ALOGE("Expected YES or NO for FORCED attribute, "
                      "got '%s' instead.",
                      val.c_str());

                return ERROR_MALFORMED;
            }

            haveGroupForced = true;
        } else if (!strcasecmp("uri", key.c_str())) {
            if (val.size() < 2
                    || val.c_str()[0] != '"'
                    || val.c_str()[val.size() - 1] != '"') {
                ALOGE("Expected quoted string for URI, got '%s' instead.",
                      val.c_str());

                return ERROR_MALFORMED;
            }

            AString tmp(val, 1, val.size() - 2);

            if (!MakeURL(mBaseURI.c_str(), tmp.c_str(), &groupURI)) {
                ALOGI("Failed to make absolute URI from '%s'.", tmp.c_str());
            }

            haveGroupURI = true;
        }
    }

    if (!haveGroupType || !haveGroupID || !haveGroupName) {
        ALOGE("Incomplete EXT-X-MEDIA element.");
        return ERROR_MALFORMED;
    }

    if (groupType == MediaGroup::TYPE_CC) {
        // TODO: ignore this for now.
        // CC track will be detected by CCDecoder. But we still need to
        // pass the CC track flags (lang, auto) to the app in the future.
        return OK;
    }

    uint32_t flags = 0;
    if (haveGroupAutoselect && groupAutoselect) {
        flags |= MediaGroup::FLAG_AUTOSELECT;
    }
    if (haveGroupDefault && groupDefault) {
        flags |= MediaGroup::FLAG_DEFAULT;
    }
    if (haveGroupForced) {
        if (groupType != MediaGroup::TYPE_SUBS) {
            ALOGE("The FORCED attribute MUST not be present on anything "
                  "but SUBS media.");

            return ERROR_MALFORMED;
        }

        if (groupForced) {
            flags |= MediaGroup::FLAG_FORCED;
        }
    }
    if (haveGroupLanguage) {
        flags |= MediaGroup::FLAG_HAS_LANGUAGE;
    }
    if (haveGroupURI) {
        flags |= MediaGroup::FLAG_HAS_URI;
    }

    ssize_t groupIndex = mMediaGroups.indexOfKey(groupID);
    sp<MediaGroup> group;

    if (groupIndex < 0) {
        group = new MediaGroup(groupType);
        mMediaGroups.add(groupID, group);
    } else {
        group = mMediaGroups.valueAt(groupIndex);

        if (group->type() != groupType) {
            ALOGE("Attempt to put media item under group of different type "
                  "(groupType = %d, item type = %d",
                  group->type(),
                  groupType);

            return ERROR_MALFORMED;
        }
    }

    return group->addMedia(
            groupName.c_str(),
            haveGroupURI ? groupURI.c_str() : NULL,
            haveGroupLanguage ? groupLanguage.c_str() : NULL,
            flags);
}

// static
status_t M3UParser::parseDiscontinuitySequence(const AString &line, size_t *seq) {
    ssize_t colonPos = line.find(":");

    if (colonPos < 0) {
        return ERROR_MALFORMED;
    }

    int32_t x;
    status_t err = ParseInt32(line.c_str() + colonPos + 1, &x);
    if (err != OK) {
        return err;
    }

    if (x < 0) {
        return ERROR_MALFORMED;
    }

    if (seq) {
        *seq = x;
    }
    return OK;
}

// static
status_t M3UParser::ParseInt32(const char *s, int32_t *x) {
    char *end;
    long lval = strtol(s, &end, 10);

    if (end == s || (*end != '\0' && *end != ',')) {
        return ERROR_MALFORMED;
    }

    *x = (int32_t)lval;

    return OK;
}

// static
status_t M3UParser::ParseDouble(const char *s, double *x) {
    char *end;
    double dval = strtod(s, &end);

    if (end == s || (*end != '\0' && *end != ',')) {
        return ERROR_MALFORMED;
    }

    *x = dval;

    return OK;
}

// static
bool M3UParser::isQuotedString(const AString &str) {
    if (str.size() < 2
            || str.c_str()[0] != '"'
            || str.c_str()[str.size() - 1] != '"') {
        return false;
    }
    return true;
}

// static
AString M3UParser::unquoteString(const AString &str) {
     if (!isQuotedString(str)) {
         return str;
     }
     return AString(str, 1, str.size() - 2);
}

// static
bool M3UParser::codecIsType(const AString &codec, const char *type) {
    if (codec.size() < 4) {
        return false;
    }
    const char *c = codec.c_str();
    switch (FOURCC(c[0], c[1], c[2], c[3])) {
        // List extracted from http://www.mp4ra.org/codecs.html
        case 'ac-3':
        case 'alac':
        case 'dra1':
        case 'dtsc':
        case 'dtse':
        case 'dtsh':
        case 'dtsl':
        case 'ec-3':
        case 'enca':
        case 'g719':
        case 'g726':
        case 'm4ae':
        case 'mlpa':
        case 'mp4a':
        case 'raw ':
        case 'samr':
        case 'sawb':
        case 'sawp':
        case 'sevc':
        case 'sqcp':
        case 'ssmv':
        case 'twos':
        case 'agsm':
        case 'alaw':
        case 'dvi ':
        case 'fl32':
        case 'fl64':
        case 'ima4':
        case 'in24':
        case 'in32':
        case 'lpcm':
        case 'Qclp':
        case 'QDM2':
        case 'QDMC':
        case 'ulaw':
        case 'vdva':
            return !strcmp("audio", type);

        case 'avc1':
        case 'avc2':
        case 'avcp':
        case 'drac':
        case 'encv':
        case 'mjp2':
        case 'mp4v':
        case 'mvc1':
        case 'mvc2':
        case 'resv':
        case 's263':
        case 'svc1':
        case 'vc-1':
        case 'CFHD':
        case 'civd':
        case 'DV10':
        case 'dvh5':
        case 'dvh6':
        case 'dvhp':
        case 'DVOO':
        case 'DVOR':
        case 'DVTV':
        case 'DVVT':
        case 'flic':
        case 'gif ':
        case 'h261':
        case 'h263':
        case 'HD10':
        case 'jpeg':
        case 'M105':
        case 'mjpa':
        case 'mjpb':
        case 'png ':
        case 'PNTG':
        case 'rle ':
        case 'rpza':
        case 'Shr0':
        case 'Shr1':
        case 'Shr2':
        case 'Shr3':
        case 'Shr4':
        case 'SVQ1':
        case 'SVQ3':
        case 'tga ':
        case 'tiff':
        case 'WRLE':
            return !strcmp("video", type);

        default:
            return false;
    }
}

}  // namespace android
