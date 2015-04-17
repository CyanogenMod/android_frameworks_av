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
#define LOG_TAG "MediaCodecListOverrides"
#include <utils/Log.h>

#include "MediaCodecListOverrides.h"

#include <gui/Surface.h>
#include <media/ICrypto.h>
#include <media/IMediaCodecList.h>
#include <media/MediaCodecInfo.h>

#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaCodec.h>

namespace android {

// a limit to avoid allocating unreasonable number of codec instances in the measurement.
// this should be in sync with the MAX_SUPPORTED_INSTANCES defined in MediaCodecInfo.java.
static const int kMaxInstances = 32;

// TODO: move MediaCodecInfo to C++. Until then, some temp methods to parse out info.
static bool getMeasureSize(sp<MediaCodecInfo::Capabilities> caps, int32_t *width, int32_t *height) {
    AString sizeRange;
    if (!caps->getDetails()->findString("size-range", &sizeRange)) {
        return false;
    }
    AString minSize;
    AString maxSize;
    if (!splitString(sizeRange, "-", &minSize, &maxSize)) {
        return false;
    }
    AString sWidth;
    AString sHeight;
    if (!splitString(minSize, "x", &sWidth, &sHeight)) {
        if (!splitString(minSize, "*", &sWidth, &sHeight)) {
            return false;
        }
    }

    *width = strtol(sWidth.c_str(), NULL, 10);
    *height = strtol(sHeight.c_str(), NULL, 10);
    return (*width > 0) && (*height > 0);
}

static void getMeasureBitrate(sp<MediaCodecInfo::Capabilities> caps, int32_t *bitrate) {
    // Until have native MediaCodecInfo, we cannot get bitrates based on profile/levels.
    // We use 200000 as default value for our measurement.
    *bitrate = 200000;
    AString bitrateRange;
    if (!caps->getDetails()->findString("bitrate-range", &bitrateRange)) {
        return;
    }
    AString minBitrate;
    AString maxBitrate;
    if (!splitString(bitrateRange, "-", &minBitrate, &maxBitrate)) {
        return;
    }

    *bitrate = strtol(minBitrate.c_str(), NULL, 10);
}

static sp<AMessage> getMeasureFormat(
        bool isEncoder, AString mime, sp<MediaCodecInfo::Capabilities> caps) {
    sp<AMessage> format = new AMessage();
    format->setString("mime", mime);

    if (isEncoder) {
        int32_t bitrate = 0;
        getMeasureBitrate(caps, &bitrate);
        format->setInt32("bitrate", bitrate);
    }

    if (mime.startsWith("video/")) {
        int32_t width = 0;
        int32_t height = 0;
        if (!getMeasureSize(caps, &width, &height)) {
            return NULL;
        }
        format->setInt32("width", width);
        format->setInt32("height", height);

        Vector<uint32_t> colorFormats;
        caps->getSupportedColorFormats(&colorFormats);
        if (colorFormats.size() == 0) {
            return NULL;
        }
        format->setInt32("color-format", colorFormats[0]);

        format->setFloat("frame-rate", 10.0);
        format->setInt32("i-frame-interval", 10);
    } else {
        // TODO: profile hw audio
        return NULL;
    }

    return format;
}

static size_t doProfileCodecs(
        bool isEncoder, AString name, AString mime, sp<MediaCodecInfo::Capabilities> caps) {
    sp<AMessage> format = getMeasureFormat(isEncoder, mime, caps);
    if (format == NULL) {
        return 0;
    }
    if (isEncoder) {
        format->setInt32("encoder", 1);
    }
    ALOGV("doProfileCodecs %s %s %s %s",
            name.c_str(), mime.c_str(), isEncoder ? "encoder" : "decoder",
            format->debugString().c_str());

    status_t err = OK;
    Vector<sp<MediaCodec>> codecs;
    while (err == OK && codecs.size() < kMaxInstances) {
        sp<ALooper> looper = new ALooper;
        looper->setName("MediaCodec_looper");
        ALOGV("doProfileCodecs for codec #%zu", codecs.size());
        ALOGV("doProfileCodecs start looper");
        looper->start(
                false /* runOnCallingThread */, false /* canCallJava */, ANDROID_PRIORITY_AUDIO);
        ALOGV("doProfileCodecs CreateByComponentName");
        sp<MediaCodec> codec = MediaCodec::CreateByComponentName(looper, name.c_str(), &err);
        if (err != OK) {
            ALOGV("Failed to create codec: %s", name.c_str());
            break;
        }
        const sp<Surface> nativeWindow;
        const sp<ICrypto> crypto;
        uint32_t flags = 0;
        ALOGV("doProfileCodecs configure");
        err = codec->configure(format, nativeWindow, crypto, flags);
        if (err != OK) {
            ALOGV("Failed to configure codec: %s with mime: %s", name.c_str(), mime.c_str());
            codec->release();
            break;
        }
        ALOGV("doProfileCodecs start");
        err = codec->start();
        if (err != OK) {
            ALOGV("Failed to start codec: %s with mime: %s", name.c_str(), mime.c_str());
            codec->release();
            break;
        }
        codecs.push_back(codec);
    }

    for (size_t i = 0; i < codecs.size(); ++i) {
        ALOGV("doProfileCodecs release %s", name.c_str());
        err = codecs[i]->release();
        if (err != OK) {
            ALOGE("Failed to release codec: %s with mime: %s", name.c_str(), mime.c_str());
        }
    }

    return codecs.size();
}

static void printLongString(const char *buf, size_t size) {
    AString print;
    const char *start = buf;
    size_t len;
    size_t totalLen = size;
    while (totalLen > 0) {
        len = (totalLen > 500) ? 500 : totalLen;
        print.setTo(start, len);
        ALOGV("%s", print.c_str());
        totalLen -= len;
        start += len;
    }
}

bool splitString(const AString &s, const AString &delimiter, AString *s1, AString *s2) {
    ssize_t pos = s.find(delimiter.c_str());
    if (pos < 0) {
        return false;
    }
    *s1 = AString(s, 0, pos);
    *s2 = AString(s, pos + 1, s.size() - pos - 1);
    return true;
}

bool splitString(
        const AString &s, const AString &delimiter, AString *s1, AString *s2, AString *s3) {
    AString temp;
    if (!splitString(s, delimiter, s1, &temp)) {
        return false;
    }
    if (!splitString(temp, delimiter, s2, s3)) {
        return false;
    }
    return true;
}

void profileCodecs(
        const Vector<sp<MediaCodecInfo>> &infos,
        KeyedVector<AString, CodecSettings> *results,
        bool forceToMeasure) {
    KeyedVector<AString, sp<MediaCodecInfo::Capabilities>> codecsNeedMeasure;
    for (size_t i = 0; i < infos.size(); ++i) {
        const sp<MediaCodecInfo> info = infos[i];
        AString name = info->getCodecName();
        if (name.startsWith("OMX.google.") ||
                // TODO: reenable below codecs once fixed
                name == "OMX.Intel.VideoDecoder.VP9.hybrid") {
            continue;
        }

        Vector<AString> mimes;
        info->getSupportedMimes(&mimes);
        for (size_t i = 0; i < mimes.size(); ++i) {
            const sp<MediaCodecInfo::Capabilities> &caps =
                    info->getCapabilitiesFor(mimes[i].c_str());
            if (!forceToMeasure && caps->getDetails()->contains("max-supported-instances")) {
                continue;
            }

            size_t max = doProfileCodecs(info->isEncoder(), name, mimes[i], caps);
            if (max > 0) {
                CodecSettings settings;
                char maxStr[32];
                sprintf(maxStr, "%zu", max);
                settings.add("max-supported-instances", maxStr);

                AString key = name;
                key.append(" ");
                key.append(mimes[i]);
                key.append(" ");
                key.append(info->isEncoder() ? "encoder" : "decoder");
                results->add(key, settings);
            }
        }
    }
}

void applyCodecSettings(
        const AString& codecInfo,
        const CodecSettings &settings,
        Vector<sp<MediaCodecInfo>> *infos) {
    AString name;
    AString mime;
    AString type;
    if (!splitString(codecInfo, " ", &name, &mime, &type)) {
        return;
    }

    for (size_t i = 0; i < infos->size(); ++i) {
        const sp<MediaCodecInfo> &info = infos->itemAt(i);
        if (name != info->getCodecName()) {
            continue;
        }

        Vector<AString> mimes;
        info->getSupportedMimes(&mimes);
        for (size_t j = 0; j < mimes.size(); ++j) {
            if (mimes[j] != mime) {
                continue;
            }
            const sp<MediaCodecInfo::Capabilities> &caps = info->getCapabilitiesFor(mime.c_str());
            for (size_t k = 0; k < settings.size(); ++k) {
                caps->getDetails()->setString(
                        settings.keyAt(k).c_str(), settings.valueAt(k).c_str());
            }
        }
    }
}

void exportResultsToXML(const char *fileName, const KeyedVector<AString, CodecSettings>& results) {
#if LOG_NDEBUG == 0
    ALOGE("measurement results");
    for (size_t i = 0; i < results.size(); ++i) {
        ALOGE("key %s", results.keyAt(i).c_str());
        const CodecSettings &settings = results.valueAt(i);
        for (size_t j = 0; j < settings.size(); ++j) {
            ALOGE("name %s value %s", settings.keyAt(j).c_str(), settings.valueAt(j).c_str());
        }
    }
#endif

    AString overrides;
    FILE *f = fopen(fileName, "rb");
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        rewind(f);

        char *buf = (char *)malloc(size);
        if (fread(buf, size, 1, f) == 1) {
            overrides.setTo(buf, size);
            if (!LOG_NDEBUG) {
                ALOGV("Existing overrides:");
                printLongString(buf, size);
            }
        } else {
            ALOGE("Failed to read %s", fileName);
        }
        fclose(f);
        free(buf);
    }

    for (size_t i = 0; i < results.size(); ++i) {
        AString name;
        AString mime;
        AString type;
        if (!splitString(results.keyAt(i), " ", &name, &mime, &type)) {
            continue;
        }
        name = AStringPrintf("\"%s\"", name.c_str());
        mime = AStringPrintf("\"%s\"", mime.c_str());
        ALOGV("name(%s) mime(%s) type(%s)", name.c_str(), mime.c_str(), type.c_str());
        ssize_t posCodec = overrides.find(name.c_str());
        size_t posInsert = 0;
        if (posCodec < 0) {
            AString encodersDecoders = (type == "encoder") ? "<Encoders>" : "<Decoders>";
            AString encodersDecodersEnd = (type == "encoder") ? "</Encoders>" : "</Decoders>";
            ssize_t posEncodersDecoders = overrides.find(encodersDecoders.c_str());
            if (posEncodersDecoders < 0) {
                AString mediaCodecs = "<MediaCodecs>";
                ssize_t posMediaCodec = overrides.find(mediaCodecs.c_str());
                if (posMediaCodec < 0) {
                    posMediaCodec = overrides.size();
                    overrides.insert("\n<MediaCodecs>\n</MediaCodecs>\n", posMediaCodec);
                    posMediaCodec = overrides.find(mediaCodecs.c_str(), posMediaCodec);
                }
                posEncodersDecoders = posMediaCodec + mediaCodecs.size();
                AString codecs = AStringPrintf(
                        "\n    %s\n    %s", encodersDecoders.c_str(), encodersDecodersEnd.c_str());
                overrides.insert(codecs.c_str(), posEncodersDecoders);
                posEncodersDecoders = overrides.find(encodersDecoders.c_str(), posEncodersDecoders);
            }
            posCodec = posEncodersDecoders + encodersDecoders.size();
            AString codec = AStringPrintf(
                        "\n        <MediaCodec name=%s type=%s update=\"true\" >\n        </MediaCodec>",
                        name.c_str(),
                        mime.c_str());
            overrides.insert(codec.c_str(), posCodec);
            posCodec = overrides.find(name.c_str());
        }

        // insert to existing entry
        ssize_t posMime = overrides.find(mime.c_str(), posCodec);
        ssize_t posEnd = overrides.find(">", posCodec);
        if (posEnd < 0) {
            ALOGE("Format error in overrides file.");
            return;
        }
        if (posMime < 0 || posMime > posEnd) {
            // new mime for an existing component
            AString codecEnd = "</MediaCodec>";
            posInsert = overrides.find(codecEnd.c_str(), posCodec) + codecEnd.size();
            AString codec = AStringPrintf(
                    "\n        <MediaCodec name=%s type=%s update=\"true\" >\n        </MediaCodec>",
                    name.c_str(),
                    mime.c_str());
            overrides.insert(codec.c_str(), posInsert);
            posInsert = overrides.find(">", posInsert) + 1;
        } else {
            posInsert = posEnd + 1;
        }

        CodecSettings settings = results.valueAt(i);
        for (size_t i = 0; i < settings.size(); ++i) {
            // WARNING: we assume all the settings are "Limit". Currently we have only one type
            // of setting in this case, which is "max-supported-instances".
            AString strInsert = AStringPrintf(
                    "\n            <Limit name=\"%s\" value=\"%s\" />",
                    settings.keyAt(i).c_str(),
                    settings.valueAt(i).c_str());
            overrides.insert(strInsert, posInsert);
        }
    }

    if (!LOG_NDEBUG) {
        ALOGV("New overrides:");
        printLongString(overrides.c_str(), overrides.size());
    }

    f = fopen(fileName, "wb");
    if (f == NULL) {
        ALOGE("Failed to open %s for writing.", fileName);
        return;
    }
    if (fwrite(overrides.c_str(), 1, overrides.size(), f) != overrides.size()) {
        ALOGE("Failed to write to %s.", fileName);
    }
    fclose(f);
}

}  // namespace android
