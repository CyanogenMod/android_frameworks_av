/*
 * Copyright (C) 2009 The Android Open Source Project
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
#define LOG_TAG "StagefrightMetadataRetriever"

#include <inttypes.h>

#include <utils/Log.h>

#include "include/StagefrightMetadataRetriever.h"

#include <media/IMediaHTTPService.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/ColorConverter.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/FileSource.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXCodec.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/Utils.h>
#include "include/ExtendedUtils.h"
#include <CharacterEncodingDetector.h>

namespace android {

StagefrightMetadataRetriever::StagefrightMetadataRetriever()
    : mParsedMetaData(false),
      mAlbumArt(NULL) {
    ALOGV("StagefrightMetadataRetriever()");

    DataSource::RegisterDefaultSniffers();
    CHECK_EQ(mClient.connect(), (status_t)OK);
}

StagefrightMetadataRetriever::~StagefrightMetadataRetriever() {
    ALOGV("~StagefrightMetadataRetriever()");

    delete mAlbumArt;
    mAlbumArt = NULL;

    mClient.disconnect();
}

status_t StagefrightMetadataRetriever::setDataSource(
        const sp<IMediaHTTPService> &httpService,
        const char *uri,
        const KeyedVector<String8, String8> *headers) {
    ALOGI("setDataSource(%s)", uriDebugString(uri, false).c_str());

    mParsedMetaData = false;
    mMetaData.clear();
    delete mAlbumArt;
    mAlbumArt = NULL;

    mSource = DataSource::CreateFromURI(httpService, uri, headers);

    if (mSource == NULL) {
        ALOGE("Unable to create data source for '%s'.", uri);
        return UNKNOWN_ERROR;
    }

    mExtractor = MediaExtractor::Create(mSource);

    if (mExtractor == NULL) {
        ALOGE("Unable to instantiate an extractor for '%s'.", uri);

        mSource.clear();

        return UNKNOWN_ERROR;
    }

    return OK;
}

// Warning caller retains ownership of the filedescriptor! Dup it if necessary.
status_t StagefrightMetadataRetriever::setDataSource(
        int fd, int64_t offset, int64_t length) {
    fd = dup(fd);

    ALOGV("setDataSource(%d, %" PRId64 ", %" PRId64 ")", fd, offset, length);
    if (fd) {
        ExtendedUtils::printFileName(fd);
    }

    mParsedMetaData = false;
    mMetaData.clear();
    delete mAlbumArt;
    mAlbumArt = NULL;

    mSource = new FileSource(fd, offset, length);

    status_t err;
    if ((err = mSource->initCheck()) != OK) {
        mSource.clear();

        return err;
    }

    mExtractor = MediaExtractor::Create(mSource);

    if (mExtractor == NULL) {
        mSource.clear();

        return UNKNOWN_ERROR;
    }

    return OK;
}

static bool isYUV420PlanarSupported(
            OMXClient *client,
            const sp<MetaData> &trackMeta) {

    const char *mime;
    CHECK(trackMeta->findCString(kKeyMIMEType, &mime));

    Vector<CodecCapabilities> caps;
    if (QueryCodecs(client->interface(), mime,
                    true, /* queryDecoders */
                    true, /* hwCodecOnly */
                    &caps) == OK) {

        for (size_t j = 0; j < caps.size(); ++j) {
            CodecCapabilities cap = caps[j];
            for (size_t i = 0; i < cap.mColorFormats.size(); ++i) {
                if (cap.mColorFormats[i] == OMX_COLOR_FormatYUV420Planar) {
                    return true;
                }
            }
        }
    }
    return false;
}

static VideoFrame *extractVideoFrameWithCodecFlags(
        OMXClient *client,
        const sp<MetaData> &trackMeta,
        const sp<MediaSource> &source,
        uint32_t flags,
        int64_t frameTimeUs,
        int seekMode) {

    sp<MetaData> format = source->getFormat();

    // XXX:
    // Once all vendors support OMX_COLOR_FormatYUV420Planar, we can
    // remove this check and always set the decoder output color format
    // skip this check for software decoders
    if (!(flags & OMXCodec::kSoftwareCodecsOnly)) {
        if (isYUV420PlanarSupported(client, trackMeta)) {
            format->setInt32(kKeyColorFormat, OMX_COLOR_FormatYUV420Planar);
        }
    }

    sp<MediaSource> decoder =
        OMXCodec::Create(
                client->interface(), format, false, source,
                NULL, flags | OMXCodec::kClientNeedsFramebuffer);

    if (decoder.get() == NULL) {
        ALOGV("unable to instantiate video decoder.");

        return NULL;
    }

    status_t err = decoder->start();
    if (err != OK) {
        ALOGW("OMXCodec::start returned error %d (0x%08x)\n", err, err);
        return NULL;
    }

    // Read one output buffer, ignore format change notifications
    // and spurious empty buffers.

    MediaSource::ReadOptions options;
    if (seekMode < MediaSource::ReadOptions::SEEK_PREVIOUS_SYNC ||
        seekMode > MediaSource::ReadOptions::SEEK_CLOSEST) {

        ALOGE("Unknown seek mode: %d", seekMode);
        return NULL;
    }

    MediaSource::ReadOptions::SeekMode mode =
            static_cast<MediaSource::ReadOptions::SeekMode>(seekMode);

    int64_t thumbNailTime;
    if (frameTimeUs < 0) {
        if (!trackMeta->findInt64(kKeyThumbnailTime, &thumbNailTime)
                || thumbNailTime < 0) {
            thumbNailTime = 0;
        }
        options.setSeekTo(thumbNailTime, mode);
    } else {
        thumbNailTime = -1;
        options.setSeekTo(frameTimeUs, mode);
    }

    MediaBuffer *buffer = NULL;
    do {
        if (buffer != NULL) {
            buffer->release();
            buffer = NULL;
        }
        err = decoder->read(&buffer, &options);
        options.clearSeekTo();
    } while (err == INFO_FORMAT_CHANGED
             || (buffer != NULL && buffer->range_length() == 0));

    if (err != OK) {
        CHECK(buffer == NULL);

        ALOGV("decoding frame failed.");
        decoder->stop();

        return NULL;
    }

    ALOGV("successfully decoded video frame.");

    int32_t unreadable;
    if (buffer->meta_data()->findInt32(kKeyIsUnreadable, &unreadable)
            && unreadable != 0) {
        ALOGV("video frame is unreadable, decoder does not give us access "
             "to the video data.");

        buffer->release();
        buffer = NULL;

        decoder->stop();

        return NULL;
    }

    int64_t timeUs;
    CHECK(buffer->meta_data()->findInt64(kKeyTime, &timeUs));
    if (thumbNailTime >= 0) {
        if (timeUs != thumbNailTime) {
            const char *mime;
            CHECK(trackMeta->findCString(kKeyMIMEType, &mime));

            ALOGV("thumbNailTime = %" PRId64 " us, timeUs = %" PRId64 " us, mime = %s",
                 thumbNailTime, timeUs, mime);
        }
    }

    sp<MetaData> meta = decoder->getFormat();

    int32_t width, height;
    CHECK(meta->findInt32(kKeyWidth, &width));
    CHECK(meta->findInt32(kKeyHeight, &height));

    int32_t crop_left, crop_top, crop_right, crop_bottom;
    if (!meta->findRect(
                kKeyCropRect,
                &crop_left, &crop_top, &crop_right, &crop_bottom)) {
        crop_left = crop_top = 0;
        crop_right = width - 1;
        crop_bottom = height - 1;
    }

    int32_t rotationAngle;
    if (!trackMeta->findInt32(kKeyRotation, &rotationAngle)) {
        rotationAngle = 0;  // By default, no rotation
    }

    VideoFrame *frame = new VideoFrame;
    frame->mWidth = crop_right - crop_left + 1;
    frame->mHeight = crop_bottom - crop_top + 1;
    frame->mDisplayWidth = frame->mWidth;
    frame->mDisplayHeight = frame->mHeight;
    frame->mSize = frame->mWidth * frame->mHeight * 2;
    frame->mData = new uint8_t[frame->mSize];
    frame->mRotationAngle = rotationAngle;

    int32_t displayWidth, displayHeight;
    if (meta->findInt32(kKeyDisplayWidth, &displayWidth)) {
        frame->mDisplayWidth = displayWidth;
    }
    if (meta->findInt32(kKeyDisplayHeight, &displayHeight)) {
        frame->mDisplayHeight = displayHeight;
    }

    int32_t srcFormat;
    CHECK(meta->findInt32(kKeyColorFormat, &srcFormat));

    ColorConverter converter(
            (OMX_COLOR_FORMATTYPE)srcFormat, OMX_COLOR_Format16bitRGB565);

    if (converter.isValid()) {
        err = converter.convert(
                (const uint8_t *)buffer->data() + buffer->range_offset(),
                width, height,
                crop_left, crop_top, crop_right, crop_bottom,
                frame->mData,
                frame->mWidth,
                frame->mHeight,
                0, 0, frame->mWidth - 1, frame->mHeight - 1);
    } else {
        ALOGE("Unable to instantiate color conversion from format 0x%08x to "
              "RGB565",
              srcFormat);

        err = ERROR_UNSUPPORTED;
    }

    buffer->release();
    buffer = NULL;

    decoder->stop();

    if (err != OK) {
        ALOGE("Colorconverter failed to convert frame.");

        delete frame;
        frame = NULL;
    }

    return frame;
}

VideoFrame *StagefrightMetadataRetriever::getFrameAtTime(
        int64_t timeUs, int option) {

    ALOGV("getFrameAtTime: %" PRId64 " us option: %d", timeUs, option);

    if (mExtractor.get() == NULL) {
        ALOGV("no extractor.");
        return NULL;
    }

    sp<MetaData> fileMeta = mExtractor->getMetaData();

    if (fileMeta == NULL) {
        ALOGV("extractor doesn't publish metadata, failed to initialize?");
        return NULL;
    }

    int32_t drm = 0;
    if (fileMeta->findInt32(kKeyIsDRM, &drm) && drm != 0) {
        ALOGE("frame grab not allowed.");
        return NULL;
    }

    size_t n = mExtractor->countTracks();
    size_t i;
    for (i = 0; i < n; ++i) {
        sp<MetaData> meta = mExtractor->getTrackMetaData(i);

        if (meta == NULL) {
            continue;
        }

        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));

        if (!strncasecmp(mime, "video/", 6)) {
            break;
        }
    }

    if (i == n) {
        ALOGV("no video track found.");
        return NULL;
    }

    sp<MetaData> trackMeta = mExtractor->getTrackMetaData(
            i, MediaExtractor::kIncludeExtensiveMetaData);

    sp<MediaSource> source = mExtractor->getTrack(i);

    if (source.get() == NULL) {
        ALOGV("unable to instantiate video track.");
        return NULL;
    }

    const void *data;
    uint32_t type;
    size_t dataSize;
    if (fileMeta->findData(kKeyAlbumArt, &type, &data, &dataSize)
            && mAlbumArt == NULL) {
        mAlbumArt = MediaAlbumArt::fromData(dataSize, data);
    }

    VideoFrame *frame =
        extractVideoFrameWithCodecFlags(
                &mClient, trackMeta, source, OMXCodec::kSoftwareCodecsOnly,
                timeUs, option);

    if (frame == NULL) {
        ALOGV("Software decoder failed to extract thumbnail, "
             "trying hardware decoder.");

        frame = extractVideoFrameWithCodecFlags(&mClient, trackMeta, source, 0,
                        timeUs, option);
    }

    return frame;
}

MediaAlbumArt *StagefrightMetadataRetriever::extractAlbumArt() {
    ALOGV("extractAlbumArt (extractor: %s)", mExtractor.get() != NULL ? "YES" : "NO");

    if (mExtractor == NULL) {
        return NULL;
    }

    if (!mParsedMetaData) {
        parseMetaData();

        mParsedMetaData = true;
    }

    if (mAlbumArt) {
        return mAlbumArt->clone();
    }

    return NULL;
}

const char *StagefrightMetadataRetriever::extractMetadata(int keyCode) {
    if (mExtractor == NULL) {
        return NULL;
    }

    if (!mParsedMetaData) {
        parseMetaData();

        mParsedMetaData = true;
    }

    ssize_t index = mMetaData.indexOfKey(keyCode);

    if (index < 0) {
        return NULL;
    }

    return mMetaData.valueAt(index).string();
}

void StagefrightMetadataRetriever::parseMetaData() {
    sp<MetaData> meta = mExtractor->getMetaData();

    if (meta == NULL) {
        ALOGV("extractor doesn't publish metadata, failed to initialize?");
        return;
    }

    struct Map {
        int from;
        int to;
        const char *name;
    };
    static const Map kMap[] = {
        { kKeyMIMEType, METADATA_KEY_MIMETYPE, NULL },
        { kKeyCDTrackNumber, METADATA_KEY_CD_TRACK_NUMBER, "tracknumber" },
        { kKeyDiscNumber, METADATA_KEY_DISC_NUMBER, "discnumber" },
        { kKeyAlbum, METADATA_KEY_ALBUM, "album" },
        { kKeyArtist, METADATA_KEY_ARTIST, "artist" },
        { kKeyAlbumArtist, METADATA_KEY_ALBUMARTIST, "albumartist" },
        { kKeyAuthor, METADATA_KEY_AUTHOR, NULL },
        { kKeyComposer, METADATA_KEY_COMPOSER, "composer" },
        { kKeyDate, METADATA_KEY_DATE, NULL },
        { kKeyGenre, METADATA_KEY_GENRE, "genre" },
        { kKeyTitle, METADATA_KEY_TITLE, "title" },
        { kKeyYear, METADATA_KEY_YEAR, "year" },
        { kKeyWriter, METADATA_KEY_WRITER, "writer" },
        { kKeyCompilation, METADATA_KEY_COMPILATION, "compilation" },
        { kKeyLocation, METADATA_KEY_LOCATION, NULL },
    };

    static const size_t kNumMapEntries = sizeof(kMap) / sizeof(kMap[0]);

    CharacterEncodingDetector *detector = new CharacterEncodingDetector();

    for (size_t i = 0; i < kNumMapEntries; ++i) {
        const char *value;
        if (meta->findCString(kMap[i].from, &value)) {
            if (kMap[i].name) {
                // add to charset detector
                detector->addTag(kMap[i].name, value);
            } else {
                // directly add to output list
                mMetaData.add(kMap[i].to, String8(value));
            }
        }
    }

    detector->detectAndConvert();
    int size = detector->size();
    if (size) {
        for (int i = 0; i < size; i++) {
            const char *name;
            const char *value;
            detector->getTag(i, &name, &value);
            for (size_t j = 0; j < kNumMapEntries; ++j) {
                if (kMap[j].name && !strcmp(kMap[j].name, name)) {
                    mMetaData.add(kMap[j].to, String8(value));
                }
            }
        }
    }
    delete detector;

    const void *data;
    uint32_t type;
    size_t dataSize;
    if (meta->findData(kKeyAlbumArt, &type, &data, &dataSize)
            && mAlbumArt == NULL) {
        mAlbumArt = MediaAlbumArt::fromData(dataSize, data);
    }

    size_t numTracks = mExtractor->countTracks();

    if (numTracks == 0) {      //If no tracks available, corrupt or not valid stream
        return;
    }

    char tmp[32];
    sprintf(tmp, "%zu", numTracks);

    mMetaData.add(METADATA_KEY_NUM_TRACKS, String8(tmp));

    bool hasAudio = false;
    bool hasVideo = false;
    int32_t videoWidth = -1;
    int32_t videoHeight = -1;
    int32_t audioBitrate = -1;
    int32_t rotationAngle = -1;

    // The overall duration is the duration of the longest track.
    int64_t maxDurationUs = 0;
    String8 timedTextLang;
    for (size_t i = 0; i < numTracks; ++i) {
        sp<MetaData> trackMeta = mExtractor->getTrackMetaData(i);
        if (trackMeta == NULL) {
            continue;
        }

        int64_t durationUs;
        if (trackMeta->findInt64(kKeyDuration, &durationUs)) {
            if (durationUs > maxDurationUs) {
                maxDurationUs = durationUs;
            }
        }

        const char *mime;
        if (trackMeta->findCString(kKeyMIMEType, &mime)) {
            if (!hasAudio && !strncasecmp("audio/", mime, 6)) {
                hasAudio = true;

                if (!trackMeta->findInt32(kKeyBitRate, &audioBitrate)) {
                    audioBitrate = -1;
                }
            } else if (!hasVideo && !strncasecmp("video/", mime, 6)) {
                hasVideo = true;

                CHECK(trackMeta->findInt32(kKeyWidth, &videoWidth));
                CHECK(trackMeta->findInt32(kKeyHeight, &videoHeight));
                if (!trackMeta->findInt32(kKeyRotation, &rotationAngle)) {
                    rotationAngle = 0;
                }
            } else if (!strcasecmp(mime, MEDIA_MIMETYPE_TEXT_3GPP)) {
                const char *lang;
                bool success = trackMeta->findCString(kKeyMediaLanguage, &lang);
                if (success) {
                    timedTextLang.append(String8(lang));
                    timedTextLang.append(String8(":"));
                } else {
                    ALOGE("No language found for timed text");
                }
            }
        }
    }

    // To save the language codes for all timed text tracks
    // If multiple text tracks present, the format will look
    // like "eng:chi"
    if (!timedTextLang.isEmpty()) {
        mMetaData.add(METADATA_KEY_TIMED_TEXT_LANGUAGES, timedTextLang);
    }

    // The duration value is a string representing the duration in ms.
    sprintf(tmp, "%" PRId64, (maxDurationUs + 500) / 1000);
    mMetaData.add(METADATA_KEY_DURATION, String8(tmp));

    if (hasAudio) {
        mMetaData.add(METADATA_KEY_HAS_AUDIO, String8("yes"));
    }

    if (hasVideo) {
        mMetaData.add(METADATA_KEY_HAS_VIDEO, String8("yes"));

        sprintf(tmp, "%d", videoWidth);
        mMetaData.add(METADATA_KEY_VIDEO_WIDTH, String8(tmp));

        sprintf(tmp, "%d", videoHeight);
        mMetaData.add(METADATA_KEY_VIDEO_HEIGHT, String8(tmp));

        sprintf(tmp, "%d", rotationAngle);
        mMetaData.add(METADATA_KEY_VIDEO_ROTATION, String8(tmp));
    }

    if (numTracks == 1 && hasAudio && audioBitrate >= 0) {
        sprintf(tmp, "%d", audioBitrate);
        mMetaData.add(METADATA_KEY_BITRATE, String8(tmp));
    } else {
        off64_t sourceSize;
        if (mSource->getSize(&sourceSize) == OK) {
            int64_t avgBitRate = (int64_t)(sourceSize * 8E6 / maxDurationUs);

            sprintf(tmp, "%" PRId64, avgBitRate);
            mMetaData.add(METADATA_KEY_BITRATE, String8(tmp));
        }
    }

    if (numTracks == 1) {
        const char *fileMIME;
        CHECK(meta->findCString(kKeyMIMEType, &fileMIME));

        if (!strcasecmp(fileMIME, "video/x-matroska")) {
            sp<MetaData> trackMeta = mExtractor->getTrackMetaData(0);
            const char *trackMIME;
            CHECK(trackMeta->findCString(kKeyMIMEType, &trackMIME));

            if (!strncasecmp("audio/", trackMIME, 6)) {
                // The matroska file only contains a single audio track,
                // rewrite its mime type.
                mMetaData.add(
                        METADATA_KEY_MIMETYPE, String8("audio/x-matroska"));
            }
        }
    }

    // To check whether the media file is drm-protected
    if (mExtractor->getDrmFlag()) {
        mMetaData.add(METADATA_KEY_IS_DRM, String8("1"));
    }
}

}  // namespace android
