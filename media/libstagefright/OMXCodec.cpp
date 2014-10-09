/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (c) 2010 - 2013, The Linux Foundation. All rights reserved.
 *
 * Not a Contribution
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
#define LOG_TAG "OMXCodec"
#include <utils/Log.h>
#define ATRACE_TAG ATRACE_TAG_VIDEO
#include <utils/Trace.h>

#include "include/AACEncoder.h"
#ifdef QCOM_HARDWARE
#include "include/MP3Decoder.h"
#endif

#include "include/ESDS.h"

#include <binder/IServiceManager.h>
#include <binder/MemoryDealer.h>
#include <binder/ProcessState.h>
#include <HardwareAPI.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/IMediaPlayerService.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/OMXCodec.h>
#include <media/stagefright/Utils.h>
#include <media/stagefright/SkipCutBuffer.h>
#include <utils/Vector.h>

#include <OMX_Audio.h>
#include <OMX_Component.h>
#ifdef QCOM_HARDWARE
#include <media/stagefright/ExtendedCodec.h>
#include "include/ExtendedUtils.h"
#include "include/ExtendedPrefetchSource.h"
#endif

#include "include/avc_utils.h"

#ifdef ENABLE_AV_ENHANCEMENTS
#include <QCMediaDefs.h>
#include <QCMetaData.h>
#include <QOMX_AudioExtensions.h>
#endif

#ifdef USE_SAMSUNG_COLORFORMAT
#include <sec_format.h>
#endif

#ifdef USE_TI_CUSTOM_DOMX
#include <OMX_TI_Video.h>
#include <OMX_TI_Index.h>
#include <OMX_TI_IVCommon.h>
#include <ctype.h>
#endif

namespace android {

#ifdef USE_SAMSUNG_COLORFORMAT
static const int OMX_SEC_COLOR_FormatNV12TPhysicalAddress = 0x7F000001;
static const int OMX_SEC_COLOR_FormatNV12LPhysicalAddress = 0x7F000002;
static const int OMX_SEC_COLOR_FormatNV12LVirtualAddress = 0x7F000003;
static const int OMX_SEC_COLOR_FormatNV12Tiled = 0x7FC00002;
static int calc_plane(int width, int height)
{
    int mbX, mbY;

    mbX = (width + 15)/16;
    mbY = (height + 15)/16;

    /* Alignment for interlaced processing */
    mbY = (mbY + 1) / 2 * 2;

    return (mbX * 16) * (mbY * 16);
}
#endif // USE_SAMSUNG_COLORFORMAT

// Treat time out as an error if we have not received any output
// buffers after 3 seconds.
const static int64_t kBufferFilledEventTimeOutNs = 3000000000LL;

// OMX Spec defines less than 50 color formats. If the query for
// color format is executed for more than kMaxColorFormatSupported,
// the query will fail to avoid looping forever.
// 1000 is more than enough for us to tell whether the omx
// component in question is buggy or not.
const static uint32_t kMaxColorFormatSupported = 1000;

#define FACTORY_CREATE(name) \
static sp<MediaSource> Make##name(const sp<MediaSource> &source) { \
    return new name(source); \
}

#define FACTORY_CREATE_ENCODER(name) \
static sp<MediaSource> Make##name(const sp<MediaSource> &source, const sp<MetaData> &meta) { \
    return new name(source, meta); \
}

#define FACTORY_REF(name) { #name, Make##name },

#ifdef QCOM_DIRECTTRACK
FACTORY_CREATE(MP3Decoder)
#endif
FACTORY_CREATE_ENCODER(AACEncoder)

static sp<MediaSource> InstantiateSoftwareEncoder(
        const char *name, const sp<MediaSource> &source,
        const sp<MetaData> &meta) {
    struct FactoryInfo {
        const char *name;
        sp<MediaSource> (*CreateFunc)(const sp<MediaSource> &, const sp<MetaData> &);
    };

    static const FactoryInfo kFactoryInfo[] = {
        FACTORY_REF(AACEncoder)
    };
    for (size_t i = 0;
         i < sizeof(kFactoryInfo) / sizeof(kFactoryInfo[0]); ++i) {
        if (!strcmp(name, kFactoryInfo[i].name)) {
            return (*kFactoryInfo[i].CreateFunc)(source, meta);
        }
    }

    return NULL;
}

#ifdef QCOM_DIRECTTRACK
static sp<MediaSource> InstantiateSoftwareDecoder(
        const char *name, const sp<MediaSource> &source) {
    struct FactoryInfo {
        const char *name;
        sp<MediaSource> (*CreateFunc)(const sp<MediaSource> &);
    };
    static const FactoryInfo kFactoryInfo[] = {
        FACTORY_REF(MP3Decoder)
    };
    for (size_t i = 0;
         i < sizeof(kFactoryInfo) / sizeof(kFactoryInfo[0]); ++i) {
        if (!strcmp(name, kFactoryInfo[i].name)) {
            return (*kFactoryInfo[i].CreateFunc)(source);
        }
    }
    return NULL;
}
#endif
#undef FACTORY_CREATE_ENCODER
#undef FACTORY_REF

#define CODEC_LOGI(x, ...) ALOGI("[%s] "x, mComponentName, ##__VA_ARGS__)
#define CODEC_LOGV(x, ...) ALOGV("[%s] "x, mComponentName, ##__VA_ARGS__)
#define CODEC_LOGE(x, ...) ALOGE("[%s] "x, mComponentName, ##__VA_ARGS__)

struct OMXCodecObserver : public BnOMXObserver {
    OMXCodecObserver() {
    }

    void setCodec(const sp<OMXCodec> &target) {
        mTarget = target;
    }

    // from IOMXObserver
    virtual void onMessage(const omx_message &msg) {
        sp<OMXCodec> codec = mTarget.promote();
        bool bYieldToConsumer = false;

        if (codec.get() != NULL) {
            Mutex::Autolock autoLock(codec->mLock);
            codec->on_message(msg);

            bYieldToConsumer = codec->mIsEncoder &&
                    !strncasecmp(codec->mMIME, "video/", 6) &&
                    (msg.type == omx_message::FILL_BUFFER_DONE ||
                    msg.type == omx_message::EMPTY_BUFFER_DONE);
            codec.clear();
        }

        // Yield the thread _outside_ the lock to enable the other
        // thread sharing the same lock to run.
        // usleep(0) seems to work better than sched_yield with threads
        // of different priorities.
        if (bYieldToConsumer) {
            usleep(0);
        }
    }

protected:
    virtual ~OMXCodecObserver() {}

private:
    wp<OMXCodec> mTarget;

    OMXCodecObserver(const OMXCodecObserver &);
    OMXCodecObserver &operator=(const OMXCodecObserver &);
};

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

static bool IsSoftwareCodec(const char *componentName) {
#ifdef DOLBY_UDC
    if (!strncmp("OMX.dolby.", componentName, 10)) {
        return true;
    }
#endif // DOLBY_UDC
    if (!strncmp("OMX.google.", componentName, 11)
        || !strncmp("OMX.ffmpeg.", componentName, 11)
        || !strncmp("OMX.PV.", componentName, 7)) {
        return true;
    }

    if (!strncmp("OMX.", componentName, 4)) {
        return false;
    }

    return true;
}

// A sort order in which OMX software codecs are first, followed
// by other (non-OMX) software codecs, followed by everything else.
static int CompareSoftwareCodecsFirst(
        const OMXCodec::CodecNameAndQuirks *elem1,
        const OMXCodec::CodecNameAndQuirks *elem2) {
    bool isOMX1 = !strncmp(elem1->mName.string(), "OMX.", 4);
    bool isOMX2 = !strncmp(elem2->mName.string(), "OMX.", 4);

    bool isSoftwareCodec1 = IsSoftwareCodec(elem1->mName.string());
    bool isSoftwareCodec2 = IsSoftwareCodec(elem2->mName.string());

    if (isSoftwareCodec1) {
        if (!isSoftwareCodec2) { return -1; }

        if (isOMX1) {
            if (isOMX2) { return 0; }

            return -1;
        } else {
            if (isOMX2) { return 0; }

            return 1;
        }

        return -1;
    }

    if (isSoftwareCodec2) {
        return 1;
    }

    return 0;
}

// static
void OMXCodec::findMatchingCodecs(
        const char *mime,
        bool createEncoder, const char *matchComponentName,
        uint32_t flags,
        Vector<CodecNameAndQuirks> *matchingCodecs) {
    matchingCodecs->clear();

    const MediaCodecList *list = MediaCodecList::getInstance();
    if (list == NULL) {
        return;
    }

    size_t index = 0;

#ifdef ENABLE_AV_ENHANCEMENTS
    if (matchComponentName && !strcmp("OMX.qcom.audio.encoder.aac", matchComponentName)) {
        matchingCodecs->add();
        CodecNameAndQuirks *entry = &matchingCodecs->editItemAt(index);
        entry->mName = String8("OMX.qcom.audio.encoder.aac");
        entry->mQuirks = 0;
        return;
    }
#endif
    for (;;) {
        ssize_t matchIndex =
            list->findCodecByType(mime, createEncoder, index);

        if (matchIndex < 0) {
            break;
        }

        index = matchIndex + 1;

        const char *componentName = list->getCodecName(matchIndex);

        // If a specific codec is requested, skip the non-matching ones.
        ALOGV("matchComponentName %s ",matchComponentName);
        if (matchComponentName && strcmp(componentName, matchComponentName)) {
            continue;
        }

        // When requesting software-only codecs, only push software codecs
        // When requesting hardware-only codecs, only push hardware codecs
        // When there is request neither for software-only nor for
        // hardware-only codecs, push all codecs
        if (((flags & kSoftwareCodecsOnly) &&   IsSoftwareCodec(componentName)) ||
            ((flags & kHardwareCodecsOnly) &&  !IsSoftwareCodec(componentName)) ||
            (!(flags & (kSoftwareCodecsOnly | kHardwareCodecsOnly)))) {

            ssize_t index = matchingCodecs->add();
            CodecNameAndQuirks *entry = &matchingCodecs->editItemAt(index);
            entry->mName = String8(componentName);
            entry->mQuirks = getComponentQuirks(list, matchIndex);

            ALOGV("matching '%s' quirks 0x%08x",
                  entry->mName.string(), entry->mQuirks);
        }
    }

    if (flags & kPreferSoftwareCodecs) {
        matchingCodecs->sort(CompareSoftwareCodecsFirst);
    }
}

// static
uint32_t OMXCodec::getComponentQuirks(
        const MediaCodecList *list, size_t index) {
    uint32_t quirks = 0;

    if (list->codecHasQuirk(
                index, "needs-flush-before-disable")) {
        quirks |= kNeedsFlushBeforeDisable;
    }
    if (list->codecHasQuirk(
                index, "requires-flush-complete-emulation")) {
        quirks |= kRequiresFlushCompleteEmulation;
    }
    if (list->codecHasQuirk(
                index, "supports-multiple-frames-per-input-buffer")) {
        quirks |= kSupportsMultipleFramesPerInputBuffer;
    }
    if (list->codecHasQuirk(
                index, "requires-allocate-on-input-ports")) {
        quirks |= kRequiresAllocateBufferOnInputPorts;
    }
    if (list->codecHasQuirk(
                index, "requires-allocate-on-output-ports")) {
        quirks |= kRequiresAllocateBufferOnOutputPorts;
    }
    if (list->codecHasQuirk(
                index, "requires-flush-before-shutdown")) {
        quirks |= kRequiresFlushBeforeShutdown;
    }
    if (list->codecHasQuirk(
                index, "output-buffers-are-unreadable")) {
        quirks |= kOutputBuffersAreUnreadable;
    }
    if (list->codecHasQuirk(
                index, "requies-loaded-to-idle-after-allocation")) {
        quirks |= kRequiresLoadedToIdleAfterAllocation;
    }
    if (list->codecHasQuirk(
                index, "requires-global-flush")) {
        quirks |= kRequiresGlobalFlush;
    }
    if (list->codecHasQuirk(
                index, "defers-output-buffer-allocation")) {
        quirks |= kDefersOutputBufferAllocation;
    }
#ifdef DOLBY_UDC
    if (list->codecHasQuirk(
                index, "needs-flush-before-disable")) {
        quirks |= kNeedsFlushBeforeDisable;
    }
    if (list->codecHasQuirk(
                index, "requires-flush-complete-emulation")) {
        quirks |= kRequiresFlushCompleteEmulation;
    }
#endif // DOLBY_UDC
#ifdef OMAP_ENHANCEMENT
    if (list->codecHasQuirk(
                index, "avoid-memcopy-input-recording-frames")) {
      quirks |= kAvoidMemcopyInputRecordingFrames;
    }
    if (list->codecHasQuirk(
                index, "input-buffer-sizes-are-bogus")) {
      quirks |= kInputBufferSizesAreBogus;
    }
#endif

#ifdef QCOM_HARDWARE
    quirks |= ExtendedCodec::getComponentQuirks(list,index);
#endif

    return quirks;
}

// static
bool OMXCodec::findCodecQuirks(const char *componentName, uint32_t *quirks) {
    const MediaCodecList *list = MediaCodecList::getInstance();

    if (list == NULL) {
        return false;
    }
#ifdef ENABLE_AV_ENHANCEMENTS
    if (componentName && !strcmp("OMX.qcom.audio.encoder.aac", componentName)) {
        *quirks = 0;
        return true;
    }
#endif

    ssize_t index = list->findCodecByName(componentName);

    if (index < 0) {
        return false;
    }

    *quirks = getComponentQuirks(list, index);

    return true;
}

// static
sp<MediaSource> OMXCodec::Create(
        const sp<IOMX> &omx,
        const sp<MetaData> &meta, bool createEncoder,
        const sp<MediaSource> &source,
        const char *matchComponentName,
        uint32_t flags,
        const sp<ANativeWindow> &nativeWindow) {
    int32_t requiresSecureBuffers;
    if (source->getFormat()->findInt32(
                kKeyRequiresSecureBuffers,
                &requiresSecureBuffers)
            && requiresSecureBuffers) {
        flags |= kIgnoreCodecSpecificData;
        flags |= kUseSecureInputBuffers;
    }

    const char *mime;
    bool success = meta->findCString(kKeyMIMEType, &mime);
    CHECK(success);

    Vector<CodecNameAndQuirks> matchingCodecs;

#ifdef QCOM_HARDWARE
    int channelCount = 0;
    int trackId = 0;
    meta->findInt32(kKeyChannelCount, &channelCount);
    source->getFormat()->findInt32(kKeyTrackID, &trackId);
    if (ExtendedCodec::useHWAACDecoder(mime, channelCount) && !createEncoder
            && trackId > 1) {
        findMatchingCodecs(mime, createEncoder,
            "OMX.qcom.audio.decoder.multiaac", flags, &matchingCodecs);
    } else {
#endif
        findMatchingCodecs(
            mime, createEncoder, matchComponentName, flags, &matchingCodecs);
#ifdef QCOM_HARDWARE
    }
#endif

    if (matchingCodecs.isEmpty()) {
        ALOGV("No matching codecs! (mime: %s, createEncoder: %s, "
                "matchComponentName: %s, flags: 0x%x)",
                mime, createEncoder ? "true" : "false", matchComponentName, flags);
        return NULL;
    }

    sp<OMXCodecObserver> observer = new OMXCodecObserver;
    IOMX::node_id node = 0;

    for (size_t i = 0; i < matchingCodecs.size(); ++i) {
        const char *componentNameBase = matchingCodecs[i].mName.string();
        uint32_t quirks = matchingCodecs[i].mQuirks;
        const char *componentName = componentNameBase;

        AString tmp;
        if (flags & kUseSecureInputBuffers) {
            tmp = componentNameBase;
            tmp.append(".secure");

            componentName = tmp.c_str();
        }

        if (createEncoder) {
            sp<MediaSource> softwareCodec =
                InstantiateSoftwareEncoder(componentName, source, meta);
            if (softwareCodec != NULL) {
                ALOGV("Successfully allocated software codec '%s'", componentName);

                return softwareCodec;
            }
        }

#ifdef QCOM_HARDWARE
        const char* ext_componentName = ExtendedCodec::overrideComponentName(quirks, meta);
        if(ext_componentName != NULL) {
          componentName = ext_componentName;
        }
#endif

        ALOGV("Attempting to allocate OMX node '%s'", componentName);

        if (!createEncoder
                && (quirks & kOutputBuffersAreUnreadable)
                && (flags & kClientNeedsFramebuffer)) {
            if (strncmp(componentName, "OMX.SEC.", 8)) {
                // For OMX.SEC.* decoders we can enable a special mode that
                // gives the client access to the framebuffer contents.

                ALOGW("Component '%s' does not give the client access to "
                     "the framebuffer contents. Skipping.",
                     componentName);

                continue;
            }
        }

        status_t err = omx->allocateNode(componentName, observer, &node);
        if (err == OK) {
            ALOGD("Successfully allocated OMX node '%s'", componentName);

            sp<OMXCodec> codec = new OMXCodec(
                    omx, node, quirks, flags,
                    createEncoder, mime, componentName,
                    source, nativeWindow);

            observer->setCodec(codec);

            err = codec->configureCodec(meta);
            if (err == OK) {
                return codec;
            }

            ALOGV("Failed to configure codec '%s'", componentName);
        }
    }

    return NULL;
}

status_t OMXCodec::parseAVCCodecSpecificData(
        const void *data, size_t size,
        unsigned *profile, unsigned *level) {
    const uint8_t *ptr = (const uint8_t *)data;

    // verify minimum size and configurationVersion == 1.
    if (size < 7 || ptr[0] != 1) {
        return ERROR_MALFORMED;
    }

    *profile = ptr[1];
    *level = ptr[3];

    // There is decodable content out there that fails the following
    // assertion, let's be lenient for now...
    // CHECK((ptr[4] >> 2) == 0x3f);  // reserved

    size_t lengthSize = 1 + (ptr[4] & 3);

    // commented out check below as H264_QVGA_500_NO_AUDIO.3gp
    // violates it...
    // CHECK((ptr[5] >> 5) == 7);  // reserved

    size_t numSeqParameterSets = ptr[5] & 31;

    ptr += 6;
    size -= 6;

    for (size_t i = 0; i < numSeqParameterSets; ++i) {
        if (size < 2) {
            return ERROR_MALFORMED;
        }

        size_t length = U16_AT(ptr);

        ptr += 2;
        size -= 2;

        if (size < length) {
            return ERROR_MALFORMED;
        }

        addCodecSpecificData(ptr, length);

        ptr += length;
        size -= length;
    }

    if (size < 1) {
        return ERROR_MALFORMED;
    }

    size_t numPictureParameterSets = *ptr;
    ++ptr;
    --size;

    for (size_t i = 0; i < numPictureParameterSets; ++i) {
        if (size < 2) {
            return ERROR_MALFORMED;
        }

        size_t length = U16_AT(ptr);

        ptr += 2;
        size -= 2;

        if (size < length) {
            return ERROR_MALFORMED;
        }

        addCodecSpecificData(ptr, length);

        ptr += length;
        size -= length;
    }

    return OK;
}

status_t OMXCodec::configureCodec(const sp<MetaData> &meta) {
    ALOGV("configureCodec protected=%d",
         (mFlags & kEnableGrallocUsageProtected) ? 1 : 0);

    if (!(mFlags & kIgnoreCodecSpecificData)) {
        uint32_t type;
        const void *data;
        size_t size;
        if (meta->findData(kKeyESDS, &type, &data, &size)) {
            ESDS esds((const char *)data, size);
            CHECK_EQ(esds.InitCheck(), (status_t)OK);

            const void *codec_specific_data;
            size_t codec_specific_data_size;
            esds.getCodecSpecificInfo(
                    &codec_specific_data, &codec_specific_data_size);

#ifdef QCOM_HARDWARE
            const char * mime_type;
            meta->findCString(kKeyMIMEType, &mime_type);
            if (strncmp(mime_type,
                        MEDIA_MIMETYPE_AUDIO_MPEG,
                        strlen(MEDIA_MIMETYPE_AUDIO_MPEG))) {
#endif
            addCodecSpecificData(
                    codec_specific_data, codec_specific_data_size);
#ifdef QCOM_HARDWARE
            }
#endif
#ifdef ENABLE_AV_ENHANCEMENTS
            ALOGV("OMXCodec::configureCodec check for DP in ESDS atom");
            if (!strncmp(mComponentName, "OMX.qcom.video.decoder.mpeg4",
                         sizeof("OMX.qcom.video.decoder.mpeg4"))) {
                bool isDP = ExtendedCodec::checkDPFromCodecSpecificData((const uint8_t*)data, size);
                if (isDP) {
                    ALOGE("H/W Decode Error: Data Partitioned bit set in the Header");
                    return BAD_VALUE;
                }
            }
#endif
        } else if (meta->findData(kKeyAVCC, &type, &data, &size)) {
            // Parse the AVCDecoderConfigurationRecord

            unsigned profile, level;
            status_t err;
            if ((err = parseAVCCodecSpecificData(
                            data, size, &profile, &level)) != OK) {
                ALOGE("Malformed AVC codec specific data.");
                return err;
            }

            CODEC_LOGI(
                    "AVC profile = %u (%s), level = %u",
                    profile, AVCProfileToString(profile), level);
#ifdef OMAP_ENHANCEMENT
            int32_t width, height;
            bool success = meta->findInt32(kKeyWidth, &width);
            success = success && meta->findInt32(kKeyHeight, &height);
            CHECK(success);
            if (!strcmp(mComponentName, "OMX.TI.720P.Decoder")
                && (profile == 0x42 /* Baseline */ && level <= 31)
                && (width * height <= 414720 /* 864x480 */)
                && (width <= 864 && height <= 864 ))
            {
                // Though this decoder can handle this profile/level,
                // we prefer to use "OMX.TI.Video.Decoder" for
                // Baseline Profile with level <=31 and sub 720p
                return ERROR_UNSUPPORTED;
            }
            if (!strcmp(mComponentName, "OMX.TI.Video.Decoder")
                && (profile != 0x42 /* Baseline */ || level > 31)) {
                // This stream exceeds the decoder's capabilities. The decoder
                // does not handle this gracefully and would clobber the heap
                // and wreak havoc instead...

                CODEC_LOGE("Profile and/or level exceed the decoder's capabilities.");
                return ERROR_UNSUPPORTED;
            }
#endif
        } else if (meta->findData(kKeyVorbisInfo, &type, &data, &size)) {
            addCodecSpecificData(data, size);

            CHECK(meta->findData(kKeyVorbisBooks, &type, &data, &size));
            addCodecSpecificData(data, size);
        } else if (meta->findData(kKeyRawCodecSpecificData, &type, &data, &size)) {
            ALOGV("OMXCodec::configureCodec found kKeyRawCodecSpecificData of size %d\n", size);
#ifdef ENABLE_AV_ENHANCEMENTS
            if (!strncmp(mComponentName, "OMX.qcom.video.decoder.mpeg4",
                         sizeof("OMX.qcom.video.decoder.mpeg4"))) {
                bool isDP = ExtendedCodec::checkDPFromCodecSpecificData((const uint8_t*)data, size);
                if (isDP) {
                    ALOGE("H/W Decode Error: Data Partitioned bit set in the Header");
                    return BAD_VALUE;
                }
            }
#endif
            addCodecSpecificData(data, size);
#ifdef QCOM_HARDWARE
        } else {
            ExtendedCodec::getRawCodecSpecificData(meta, data, size);
            if (size) {
                addCodecSpecificData(data, size);
            }
#endif
        }
    }

    int32_t bitRate = 0;
    if (mIsEncoder) {
        CHECK(meta->findInt32(kKeyBitRate, &bitRate));
    }
#ifdef OMAP_ENHANCEMENT
        if (!strcmp(mComponentName, "OMX.TI.Video.encoder")) {
            int32_t width, height;
            bool success = meta->findInt32(kKeyWidth, &width);
            success = success && meta->findInt32(kKeyHeight, &height);
            CHECK(success);
            if (width * height > 414720 /* 864x480 */) {
                // require OMX.TI.720P.Encoder
                return ERROR_UNSUPPORTED;
            }
        }
#endif
    if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_NB, mMIME)) {
        setAMRFormat(false /* isWAMR */, bitRate);
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_WB, mMIME)) {
        setAMRFormat(true /* isWAMR */, bitRate);
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AAC, mMIME)) {
        int32_t numChannels, sampleRate, aacProfile;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));

        if (!meta->findInt32(kKeyAACProfile, &aacProfile)) {
            aacProfile = OMX_AUDIO_AACObjectNull;
        }

        int32_t isADTS;
        if (!meta->findInt32(kKeyIsADTS, &isADTS)) {
            isADTS = false;
        }

        status_t err = setAACFormat(numChannels, sampleRate, bitRate, aacProfile, isADTS);
        if (err != OK) {
            CODEC_LOGE("setAACFormat() failed (err = %d)", err);
            return err;
        }
#ifdef ENABLE_AV_ENHANCEMENTS
        uint32_t type;
        const void *data;
        size_t size;
        if (meta->findData(kKeyAacCodecSpecificData, &type, &data, &size)) {
            ALOGV("OMXCodec:: configureCodec found kKeyAacCodecSpecificData of size %d\n", size);
            addCodecSpecificData(data, size);
        }
#endif
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_MPEG, mMIME)) {
        int32_t numChannels, sampleRate;
        if (meta->findInt32(kKeyChannelCount, &numChannels)
                && meta->findInt32(kKeySampleRate, &sampleRate)) {
            // Since we did not always check for these, leave them optional
            // and have the decoder figure it all out.
            setRawAudioFormat(
                    mIsEncoder ? kPortIndexInput : kPortIndexOutput,
                    sampleRate,
                    numChannels);
        }
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_G711_ALAW, mMIME)
            || !strcasecmp(MEDIA_MIMETYPE_AUDIO_G711_MLAW, mMIME)) {
        // These are PCM-like formats with a fixed sample rate but
        // a variable number of channels.

        int32_t numChannels;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));

        setG711Format(numChannels);
    } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_RAW, mMIME)) {
        CHECK(!mIsEncoder);

        int32_t numChannels, sampleRate;
        CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
        CHECK(meta->findInt32(kKeySampleRate, &sampleRate));

        setRawAudioFormat(kPortIndexInput, sampleRate, numChannels);
    } else if (!strncmp(mComponentName, "OMX.ffmpeg.", 11)) {
        if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_WMA, mMIME))  {
            status_t err = setWMAFormat(meta);
            if (err != OK) {
                CODEC_LOGE("setWMAFormat() failed (err = %d)", err);
                return err;
            }
        } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_VORBIS, mMIME))  {
            status_t err = setVORBISFormat(meta);
            if (err != OK) {
                CODEC_LOGE("setVORBISFormat() failed (err = %d)", err);
                return err;
            }
        } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_RA, mMIME))  {
            status_t err = setRAFormat(meta);
            if (err != OK) {
                CODEC_LOGE("setRAFormat() failed (err = %d)", err);
                return err;
            }
        } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_FLAC, mMIME))  {
            status_t err = setFLACFormat(meta);
            if (err != OK) {
                CODEC_LOGE("setFLACFormat() failed (err = %d)", err);
                return err;
            }
        } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II, mMIME))  {
            status_t err = setMP2Format(meta);
            if (err != OK) {
                CODEC_LOGE("setMP2Format() failed (err = %d)", err);
                return err;
            }
        } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AC3, mMIME) ||
                   !strcasecmp(MEDIA_MIMETYPE_AUDIO_EAC3, mMIME)) {
            status_t err = setAC3Format(meta);
            if (err != OK) {
                CODEC_LOGE("setAC3Format() failed (err = %d)", err);
                return err;
            }
#ifdef ENABLE_AV_ENHANCEMENTS
            // FFMPEG will convert floating point to 24-bit PCM
            if (ExtendedUtils::isHiresAudioEnabled()) {
                meta->setInt32(kKeySampleBits, 24);
            }
#endif
        } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_APE, mMIME))  {
            status_t err = setAPEFormat(meta);
            if (err != OK) {
                CODEC_LOGE("setAPEFormat() failed (err = %d)", err);
                return err;
            }
        } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_DTS, mMIME))  {
            status_t err = setDTSFormat(meta);
            if (err != OK) {
                CODEC_LOGE("setDTSFormat() failed (err = %d)", err);
                return err;
            }
#ifdef ENABLE_AV_ENHANCEMENTS
            // FFMPEG will convert DTS floating point to 24-bit PCM
            if (ExtendedUtils::isHiresAudioEnabled()) {
                meta->setInt32(kKeySampleBits, 24);
            }
#endif
        } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_FFMPEG, mMIME))  {
            status_t err = setFFmpegAudioFormat(meta);
            if (err != OK) {
                CODEC_LOGE("setFFmpegAudioFormat() failed (err = %d)", err);
                return err;
            }
        }
#ifdef QCOM_HARDWARE
    } else if (!strncmp(mComponentName, "OMX.qcom.", 9)) {
        if (!mIsVideo) {
            if (mIsEncoder) {
                int32_t numChannels, sampleRate;
                CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
                CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
                setRawAudioFormat(kPortIndexInput, sampleRate, numChannels);
            }
            status_t err = ExtendedCodec::setAudioFormat(
                    meta, mMIME, mOMX, mNode, mIsEncoder);
            if(OK != err) {
                return err;
            }
        }
#endif
    }

    if (!strncasecmp(mMIME, "video/", 6)) {

        if (mIsEncoder) {
            status_t err = setVideoInputFormat(mMIME, meta);
            if (err != OK) {
                return err;
            }
        } else {
#ifdef QCOM_HARDWARE
            if (mNativeWindow != NULL
                && !strncmp(mComponentName, "OMX.", 4)) {
                status_t err = initNativeWindow();
                if (err != OK) {
                    return err;
                }
            }

            ExtendedCodec::configureVideoDecoder(
                    meta, mMIME, mOMX, mFlags, mNode, mComponentName);
#endif
            status_t err = setVideoOutputFormat(
                    mMIME, meta);

            if (err != OK) {
                return err;
            }

#ifdef QCOM_HARDWARE
            ExtendedCodec::configureFramePackingFormat(
                    meta, mOMX, mNode, mComponentName);
            ExtendedCodec::enableSmoothStreaming(
                    mOMX, mNode, &mInSmoothStreamingMode, mComponentName);
#endif
        }
    }

    int32_t maxInputSize;
    if (meta->findInt32(kKeyMaxInputSize, &maxInputSize)) {
        setMinBufferSize(kPortIndexInput, (OMX_U32)maxInputSize);
    }

    initOutputFormat(meta);

    if ((mFlags & kClientNeedsFramebuffer)
            && !strncmp(mComponentName, "OMX.SEC.", 8)) {
        // This appears to no longer be needed???

        OMX_INDEXTYPE index;

        status_t err =
            mOMX->getExtensionIndex(
                    mNode,
                    "OMX.SEC.index.ThumbnailMode",
                    &index);

        if (err != OK) {
            return err;
        }

        OMX_BOOL enable = OMX_TRUE;
        err = mOMX->setConfig(mNode, index, &enable, sizeof(enable));

        if (err != OK) {
            CODEC_LOGE("setConfig('OMX.SEC.index.ThumbnailMode') "
                       "returned error 0x%08x", err);

            return err;
        }

        mQuirks &= ~kOutputBuffersAreUnreadable;
    }

#ifndef QCOM_HARDWARE
    if (mNativeWindow != NULL
        && !mIsEncoder
        && !strncasecmp(mMIME, "video/", 6)
        && !strncmp(mComponentName, "OMX.", 4)) {
        status_t err = initNativeWindow();
        if (err != OK) {
            return err;
        }
    }
#endif

    return OK;
}

void OMXCodec::setMinBufferSize(OMX_U32 portIndex, OMX_U32 size) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    if ((portIndex == kPortIndexInput && (mQuirks & kInputBufferSizesAreBogus))
        || (def.nBufferSize < size)) {
        def.nBufferSize = size;
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    // Make sure the setting actually stuck.
    if (portIndex == kPortIndexInput
            && (mQuirks & kInputBufferSizesAreBogus)) {
        CHECK_EQ(def.nBufferSize, size);
    } else {
        CHECK(def.nBufferSize >= size);
    }
}

status_t OMXCodec::setVideoPortFormatType(
        OMX_U32 portIndex,
        OMX_VIDEO_CODINGTYPE compressionFormat,
        OMX_COLOR_FORMATTYPE colorFormat) {
    OMX_VIDEO_PARAM_PORTFORMATTYPE format;
    InitOMXParams(&format);
    format.nPortIndex = portIndex;
    format.nIndex = 0;
    bool found = false;

    OMX_U32 index = 0;
    for (;;) {
        format.nIndex = index;
        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamVideoPortFormat,
                &format, sizeof(format));

        if (err != OK) {
            return err;
        }

        // The following assertion is violated by TI's video decoder.
        // CHECK_EQ(format.nIndex, index);

#if 1
        CODEC_LOGV("portIndex: %ld, index: %ld, eCompressionFormat=0x%x eColorFormat=0x%x",
             portIndex,
             index, format.eCompressionFormat, format.eColorFormat);
#endif

        if (!strcmp("OMX.TI.Video.encoder", mComponentName) ||
            !strcmp("OMX.TI.720P.Encoder", mComponentName)) {
            if (portIndex == kPortIndexInput
                    && colorFormat == format.eColorFormat) {
                // eCompressionFormat does not seem right.
                found = true;
                break;
            }
            if (portIndex == kPortIndexOutput
                    && compressionFormat == format.eCompressionFormat) {
                // eColorFormat does not seem right.
                found = true;
                break;
            }
        }

        if (format.eCompressionFormat == compressionFormat
                && format.eColorFormat == colorFormat) {
            found = true;
            break;
        }

        ++index;
        if (index >= kMaxColorFormatSupported) {
            CODEC_LOGE("color format %d or compression format %d is not supported",
                colorFormat, compressionFormat);
            return UNKNOWN_ERROR;
        }
    }

    if (!found) {
        CODEC_LOGE("not found a match.");
        return UNKNOWN_ERROR;
    }

    CODEC_LOGV("found a match.");
    status_t err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoPortFormat,
            &format, sizeof(format));

    return err;
}

#ifdef USE_SAMSUNG_COLORFORMAT
#define ALIGN_TO_8KB(x)   ((((x) + (1 << 13) - 1) >> 13) << 13)
#define ALIGN_TO_32B(x)   ((((x) + (1 <<  5) - 1) >>  5) <<  5)
#define ALIGN_TO_128B(x)  ((((x) + (1 <<  7) - 1) >>  7) <<  7)
#define ALIGN(x, a)       (((x) + (a) - 1) & ~((a) - 1))
#endif

static size_t getFrameSize(
        OMX_COLOR_FORMATTYPE colorFormat, int32_t width, int32_t height) {
    switch (colorFormat) {
        case OMX_COLOR_FormatYCbYCr:
        case OMX_COLOR_FormatCbYCrY:
            return width * height * 2;

        case OMX_COLOR_FormatYUV420Planar:
        case OMX_COLOR_FormatYUV420SemiPlanar:
        case OMX_TI_COLOR_FormatYUV420PackedSemiPlanar:
        /*
        * FIXME: For the Opaque color format, the frame size does not
        * need to be (w*h*3)/2. It just needs to
        * be larger than certain minimum buffer size. However,
        * currently, this opaque foramt has been tested only on
        * YUV420 formats. If that is changed, then we need to revisit
        * this part in the future
        */
        case OMX_COLOR_FormatAndroidOpaque:
#ifdef USE_SAMSUNG_COLORFORMAT
        case OMX_SEC_COLOR_FormatNV12TPhysicalAddress:
        case OMX_SEC_COLOR_FormatNV12LPhysicalAddress:
#endif
            return (width * height * 3) / 2;
#ifdef USE_SAMSUNG_COLORFORMAT
        case OMX_SEC_COLOR_FormatNV12LVirtualAddress:
            return ALIGN((ALIGN(width, 16) * ALIGN(height, 16)), 2048) + ALIGN((ALIGN(width, 16) * ALIGN(height >> 1, 8)), 2048);
        case OMX_SEC_COLOR_FormatNV12Tiled:
            static unsigned int frameBufferYSise = ALIGN_TO_8KB(ALIGN_TO_128B(width) * ALIGN_TO_32B(height));
            static unsigned int frameBufferUVSise = ALIGN_TO_8KB(ALIGN_TO_128B(width) * ALIGN_TO_32B(height/2));
            return (frameBufferYSise + frameBufferUVSise);
#endif
        default:
            CHECK(!"Should not be here. Unsupported color format.");
            break;
    }
}

status_t OMXCodec::findTargetColorFormat(
        const sp<MetaData>& meta, OMX_COLOR_FORMATTYPE *colorFormat) {
    ALOGV("findTargetColorFormat");
    CHECK(mIsEncoder);

    *colorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
    int32_t targetColorFormat;
    if (meta->findInt32(kKeyColorFormat, &targetColorFormat)) {
        *colorFormat = (OMX_COLOR_FORMATTYPE) targetColorFormat;
    }

    // Check whether the target color format is supported.
    return isColorFormatSupported(*colorFormat, kPortIndexInput);
}

status_t OMXCodec::isColorFormatSupported(
        OMX_COLOR_FORMATTYPE colorFormat, int portIndex) {
    ALOGV("isColorFormatSupported: %d", static_cast<int>(colorFormat));

    // Enumerate all the color formats supported by
    // the omx component to see whether the given
    // color format is supported.
    OMX_VIDEO_PARAM_PORTFORMATTYPE portFormat;
    InitOMXParams(&portFormat);
    portFormat.nPortIndex = portIndex;
    OMX_U32 index = 0;
    portFormat.nIndex = index;
    while (true) {
        if (OMX_ErrorNone != mOMX->getParameter(
                mNode, OMX_IndexParamVideoPortFormat,
                &portFormat, sizeof(portFormat))) {
            break;
        }
        // Make sure that omx component does not overwrite
        // the incremented index (bug 2897413).
        CHECK_EQ(index, portFormat.nIndex);
        if (portFormat.eColorFormat == colorFormat) {
            CODEC_LOGE("Found supported color format: %d", portFormat.eColorFormat);
            return OK;  // colorFormat is supported!
        }
        ++index;
        portFormat.nIndex = index;

        if (index >= kMaxColorFormatSupported) {
            CODEC_LOGE("More than %ld color formats are supported???", index);
            break;
        }
    }

    CODEC_LOGE("color format %d is not supported", colorFormat);
    return UNKNOWN_ERROR;
}

status_t OMXCodec::setVideoInputFormat(
        const char *mime, const sp<MetaData>& meta) {

    int32_t width, height, frameRate, bitRate, stride, sliceHeight;
    bool success = meta->findInt32(kKeyWidth, &width);
    success = success && meta->findInt32(kKeyHeight, &height);
    success = success && meta->findInt32(kKeyFrameRate, &frameRate);
    success = success && meta->findInt32(kKeyBitRate, &bitRate);
    success = success && meta->findInt32(kKeyStride, &stride);
    success = success && meta->findInt32(kKeySliceHeight, &sliceHeight);
    CHECK(success);
    CHECK(stride != 0);

    OMX_VIDEO_CODINGTYPE compressionFormat = OMX_VIDEO_CodingUnused;
    if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime)) {
        compressionFormat = OMX_VIDEO_CodingAVC;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG4, mime)) {
        compressionFormat = OMX_VIDEO_CodingMPEG4;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_H263, mime)) {
        compressionFormat = OMX_VIDEO_CodingH263;
    } else {
#ifdef QCOM_HARDWARE
        status_t err = ExtendedCodec::setVideoInputFormat(mime, &compressionFormat);
        if(err != OK) {
#endif
        ALOGE("Not a supported video mime type: %s", mime);
        CHECK(!"Should not be here. Not a supported video mime type.");
#ifdef QCOM_HARDWARE
        }
#endif
    }

    OMX_COLOR_FORMATTYPE colorFormat;
    CHECK_EQ((status_t)OK, findTargetColorFormat(meta, &colorFormat));

    status_t err;
    OMX_PARAM_PORTDEFINITIONTYPE def;
    OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;

    //////////////////////// Input port /////////////////////////
    err = setVideoPortFormatType(
            kPortIndexInput, OMX_VIDEO_CodingUnused, colorFormat);
    if(err != OK) {
        ALOGE("Setting OMX_VIDEO_CodingUnused failed");
        return err;
    }

    InitOMXParams(&def);
    def.nPortIndex = kPortIndexInput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    if(err != OK) {
        ALOGE("Getting OMX_IndexParamPortDefinition failed");
        return err;
    }

    def.nBufferSize = getFrameSize(colorFormat,
            stride > 0? stride: -stride, sliceHeight);

    if((int)def.eDomain != (int)OMX_PortDomainVideo) {
        ALOGE("Input port: Not a Video Domain!!");
        return UNKNOWN_ERROR;
    }

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;
    video_def->nStride = stride;
    video_def->nSliceHeight = sliceHeight;
    video_def->xFramerate = (frameRate << 16);  // Q16 format
    video_def->eCompressionFormat = OMX_VIDEO_CodingUnused;
    video_def->eColorFormat = colorFormat;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    if(err != OK) {
        ALOGE("Setting Video InPort Definition failed");
        return err;
    }

    //////////////////////// Output port /////////////////////////
    err = setVideoPortFormatType(
            kPortIndexOutput, compressionFormat, OMX_COLOR_FormatUnused);
    if(err != OK) {
        ALOGE("Setting compressionFormat failed");
        return err;
    }

    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    if(err != OK) {
        ALOGE("Getting Video InPort Definition failed");
        return err;
    }

    if((int)def.eDomain != (int)OMX_PortDomainVideo) {
        ALOGE("Output port: Not a Video Domain");
        return UNKNOWN_ERROR;
    }

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;
    video_def->xFramerate = 0;      // No need for output port
    video_def->nBitrate = bitRate;  // Q16 format
    video_def->eCompressionFormat = compressionFormat;
    video_def->eColorFormat = OMX_COLOR_FormatUnused;
    if (mQuirks & kRequiresLargerEncoderOutputBuffer) {
        // Increases the output buffer size
        def.nBufferSize = ((def.nBufferSize * 3) >> 1);
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    if(err != OK) {
        ALOGE("Setting Video OutPort Definition failed");
        return err;
    }

    /////////////////// Codec-specific ////////////////////////
    switch (compressionFormat) {
        case OMX_VIDEO_CodingMPEG4:
        {
            CHECK_EQ(setupMPEG4EncoderParameters(meta), (status_t)OK);
            break;
        }

        case OMX_VIDEO_CodingH263:
            CHECK_EQ(setupH263EncoderParameters(meta), (status_t)OK);
            break;

        case OMX_VIDEO_CodingAVC:
        {
            CHECK_EQ(setupAVCEncoderParameters(meta), (status_t)OK);
            break;
        }

        default:
            CHECK(!"Support for this compressionFormat to be implemented.");
            break;
    }
    return OK;
}

static OMX_U32 setPFramesSpacing(int32_t iFramesInterval, int32_t frameRate) {
    if (iFramesInterval < 0) {
        return 0xFFFFFFFF;
    } else if (iFramesInterval == 0) {
        return 0;
    }
    OMX_U32 ret = frameRate * iFramesInterval - 1;
    CHECK(ret > 1);
    return ret;
}

status_t OMXCodec::setupErrorCorrectionParameters() {
    OMX_VIDEO_PARAM_ERRORCORRECTIONTYPE errorCorrectionType;
    InitOMXParams(&errorCorrectionType);
    errorCorrectionType.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoErrorCorrection,
            &errorCorrectionType, sizeof(errorCorrectionType));
    if (err != OK) {
        ALOGW("Error correction param query is not supported");
        return OK;  // Optional feature. Ignore this failure
    }

    errorCorrectionType.bEnableHEC = OMX_FALSE;
    errorCorrectionType.bEnableResync = OMX_FALSE;
    errorCorrectionType.nResynchMarkerSpacing = 0;
    errorCorrectionType.bEnableDataPartitioning = OMX_FALSE;
    errorCorrectionType.bEnableRVLC = OMX_FALSE;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoErrorCorrection,
            &errorCorrectionType, sizeof(errorCorrectionType));
    if (err != OK) {
        ALOGW("Error correction param configuration is not supported");
    }

    // Optional feature. Ignore the failure.
    return OK;
}

status_t OMXCodec::setupBitRate(int32_t bitRate) {
    OMX_VIDEO_PARAM_BITRATETYPE bitrateType;
    InitOMXParams(&bitrateType);
    bitrateType.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoBitrate,
            &bitrateType, sizeof(bitrateType));
    CHECK_EQ(err, (status_t)OK);

    bitrateType.eControlRate = OMX_Video_ControlRateVariable;
    bitrateType.nTargetBitrate = bitRate;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoBitrate,
            &bitrateType, sizeof(bitrateType));
    CHECK_EQ(err, (status_t)OK);
    return OK;
}

status_t OMXCodec::getVideoProfileLevel(
        const sp<MetaData>& meta,
        const CodecProfileLevel& defaultProfileLevel,
        CodecProfileLevel &profileLevel) {
    CODEC_LOGV("Default profile: %ld, level %ld",
            defaultProfileLevel.mProfile, defaultProfileLevel.mLevel);

    // Are the default profile and level overwriten?
    int32_t profile, level;
    if (!meta->findInt32(kKeyVideoProfile, &profile)) {
        profile = defaultProfileLevel.mProfile;
    }
    if (!meta->findInt32(kKeyVideoLevel, &level)) {
        level = defaultProfileLevel.mLevel;
    }
    CODEC_LOGV("Target profile: %d, level: %d", profile, level);

    // Are the target profile and level supported by the encoder?
    OMX_VIDEO_PARAM_PROFILELEVELTYPE param;
    InitOMXParams(&param);
    param.nPortIndex = kPortIndexOutput;
    for (param.nProfileIndex = 0;; ++param.nProfileIndex) {
        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamVideoProfileLevelQuerySupported,
                &param, sizeof(param));

        if (err != OK) break;

        int32_t supportedProfile = static_cast<int32_t>(param.eProfile);
        int32_t supportedLevel = static_cast<int32_t>(param.eLevel);
        CODEC_LOGV("Supported profile: %d, level %d",
            supportedProfile, supportedLevel);

        if (profile == supportedProfile &&
            level <= supportedLevel) {
            // We can further check whether the level is a valid
            // value; but we will leave that to the omx encoder component
            // via OMX_SetParameter call.
            profileLevel.mProfile = profile;
            profileLevel.mLevel = level;
            return OK;
        }
    }

    CODEC_LOGE("Target profile (%d) and level (%d) is not supported",
            profile, level);
    return BAD_VALUE;
}

status_t OMXCodec::setupH263EncoderParameters(const sp<MetaData>& meta) {
    int32_t iFramesInterval, frameRate, bitRate;
    bool success = meta->findInt32(kKeyBitRate, &bitRate);
    success = success && meta->findInt32(kKeyFrameRate, &frameRate);
    success = success && meta->findInt32(kKeyIFramesInterval, &iFramesInterval);
    CHECK(success);
    OMX_VIDEO_PARAM_H263TYPE h263type;
    InitOMXParams(&h263type);
    h263type.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoH263, &h263type, sizeof(h263type));
    CHECK_EQ(err, (status_t)OK);

    h263type.nAllowedPictureTypes =
        OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;

    h263type.nPFrames = setPFramesSpacing(iFramesInterval, frameRate);
    if (h263type.nPFrames == 0) {
        h263type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
    }
    h263type.nBFrames = 0;

    // Check profile and level parameters
    CodecProfileLevel defaultProfileLevel, profileLevel;
    defaultProfileLevel.mProfile = h263type.eProfile;
    defaultProfileLevel.mLevel = h263type.eLevel;
    err = getVideoProfileLevel(meta, defaultProfileLevel, profileLevel);
    if (err != OK) return err;
    h263type.eProfile = static_cast<OMX_VIDEO_H263PROFILETYPE>(profileLevel.mProfile);
    h263type.eLevel = static_cast<OMX_VIDEO_H263LEVELTYPE>(profileLevel.mLevel);

    h263type.bPLUSPTYPEAllowed = OMX_FALSE;
    h263type.bForceRoundingTypeToZero = OMX_FALSE;
    h263type.nPictureHeaderRepetition = 0;
    h263type.nGOBHeaderInterval = 0;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoH263, &h263type, sizeof(h263type));
    CHECK_EQ(err, (status_t)OK);

    CHECK_EQ(setupBitRate(bitRate), (status_t)OK);
    CHECK_EQ(setupErrorCorrectionParameters(), (status_t)OK);

    return OK;
}

status_t OMXCodec::setupMPEG4EncoderParameters(const sp<MetaData>& meta) {
    int32_t iFramesInterval, frameRate, bitRate;
    bool success = meta->findInt32(kKeyBitRate, &bitRate);
    success = success && meta->findInt32(kKeyFrameRate, &frameRate);
    success = success && meta->findInt32(kKeyIFramesInterval, &iFramesInterval);
    CHECK(success);
    OMX_VIDEO_PARAM_MPEG4TYPE mpeg4type;
    InitOMXParams(&mpeg4type);
    mpeg4type.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoMpeg4, &mpeg4type, sizeof(mpeg4type));
    CHECK_EQ(err, (status_t)OK);

    mpeg4type.nSliceHeaderSpacing = 0;
    mpeg4type.bSVH = OMX_FALSE;
    mpeg4type.bGov = OMX_FALSE;

    mpeg4type.nAllowedPictureTypes =
        OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;

    mpeg4type.nPFrames = setPFramesSpacing(iFramesInterval, frameRate);
    if (mpeg4type.nPFrames == 0) {
        mpeg4type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
    }
    mpeg4type.nBFrames = 0;
    mpeg4type.nIDCVLCThreshold = 0;
    mpeg4type.bACPred = OMX_TRUE;
    mpeg4type.nMaxPacketSize = 256;
    mpeg4type.nTimeIncRes = 1000;
    mpeg4type.nHeaderExtension = 0;
    mpeg4type.bReversibleVLC = OMX_FALSE;

    // Check profile and level parameters
    CodecProfileLevel defaultProfileLevel, profileLevel;
    defaultProfileLevel.mProfile = mpeg4type.eProfile;
    defaultProfileLevel.mLevel = mpeg4type.eLevel;
    err = getVideoProfileLevel(meta, defaultProfileLevel, profileLevel);
    if (err != OK) return err;
    mpeg4type.eProfile = static_cast<OMX_VIDEO_MPEG4PROFILETYPE>(profileLevel.mProfile);
    mpeg4type.eLevel = static_cast<OMX_VIDEO_MPEG4LEVELTYPE>(profileLevel.mLevel);

#ifdef QCOM_HARDWARE
    ExtendedUtils::setBFrames(mpeg4type, mNumBFrames, mComponentName);
#endif
    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoMpeg4, &mpeg4type, sizeof(mpeg4type));
    CHECK_EQ(err, (status_t)OK);

    CHECK_EQ(setupBitRate(bitRate), (status_t)OK);
    CHECK_EQ(setupErrorCorrectionParameters(), (status_t)OK);

    return OK;
}

status_t OMXCodec::setupAVCEncoderParameters(const sp<MetaData>& meta) {
    int32_t iFramesInterval, frameRate, bitRate;
    bool success = meta->findInt32(kKeyBitRate, &bitRate);
    success = success && meta->findInt32(kKeyFrameRate, &frameRate);
    success = success && meta->findInt32(kKeyIFramesInterval, &iFramesInterval);
    CHECK(success);

    OMX_VIDEO_PARAM_AVCTYPE h264type;
    InitOMXParams(&h264type);
    h264type.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoAvc, &h264type, sizeof(h264type));
    CHECK_EQ(err, (status_t)OK);

    h264type.nAllowedPictureTypes =
        OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;

    // Check profile and level parameters
    CodecProfileLevel defaultProfileLevel, profileLevel;
    defaultProfileLevel.mProfile = h264type.eProfile;
    defaultProfileLevel.mLevel = h264type.eLevel;
    err = getVideoProfileLevel(meta, defaultProfileLevel, profileLevel);
    if (err != OK) return err;
    h264type.eProfile = static_cast<OMX_VIDEO_AVCPROFILETYPE>(profileLevel.mProfile);
    h264type.eLevel = static_cast<OMX_VIDEO_AVCLEVELTYPE>(profileLevel.mLevel);

    // XXX
#ifdef USE_TI_DUCATI_H264_PROFILE
    if ((strncmp(mComponentName, "OMX.TI.DUCATI1", 14) != 0)
            && (h264type.eProfile != OMX_VIDEO_AVCProfileBaseline)) {
#elif defined (QCOM_HARDWARE)
    if (ExtendedUtils::isAVCProfileSupported(h264type.eProfile)){
        ALOGI("Profile type is  %d ",h264type.eProfile);
    } else if (h264type.eProfile != OMX_VIDEO_AVCProfileBaseline) {
#else
    if (h264type.eProfile != OMX_VIDEO_AVCProfileBaseline) {
#endif
        ALOGW("Use baseline profile instead of %d for AVC recording",
            h264type.eProfile);
        h264type.eProfile = OMX_VIDEO_AVCProfileBaseline;
    }

    if (h264type.eProfile == OMX_VIDEO_AVCProfileBaseline) {
        h264type.nSliceHeaderSpacing = 0;
        h264type.bUseHadamard = OMX_TRUE;
        h264type.nRefFrames = 1;
        h264type.nBFrames = 0;
        h264type.nPFrames = setPFramesSpacing(iFramesInterval, frameRate);
        if (h264type.nPFrames == 0) {
            h264type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
        }
        h264type.nRefIdx10ActiveMinus1 = 0;
        h264type.nRefIdx11ActiveMinus1 = 0;
        h264type.bEntropyCodingCABAC = OMX_FALSE;
        h264type.bWeightedPPrediction = OMX_FALSE;
        h264type.bconstIpred = OMX_FALSE;
        h264type.bDirect8x8Inference = OMX_FALSE;
        h264type.bDirectSpatialTemporal = OMX_FALSE;
        h264type.nCabacInitIdc = 0;
    }

#ifdef QCOM_HARDWARE
    ExtendedUtils::setBFrames(
            h264type, mNumBFrames, iFramesInterval, frameRate, mComponentName);
#endif
    if (h264type.nBFrames != 0) {
        h264type.nAllowedPictureTypes |= OMX_VIDEO_PictureTypeB;
    }

    h264type.bEnableUEP = OMX_FALSE;
    h264type.bEnableFMO = OMX_FALSE;
    h264type.bEnableASO = OMX_FALSE;
    h264type.bEnableRS = OMX_FALSE;
    h264type.bFrameMBsOnly = OMX_TRUE;
    h264type.bMBAFF = OMX_FALSE;
    h264type.eLoopFilterMode = OMX_VIDEO_AVCLoopFilterEnable;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoAvc, &h264type, sizeof(h264type));
    CHECK_EQ(err, (status_t)OK);

    CHECK_EQ(setupBitRate(bitRate), (status_t)OK);

    return OK;
}

status_t OMXCodec::setVideoOutputFormat(
        const char *mime, const sp<MetaData>& meta) {

    int32_t width, height;
    bool success = meta->findInt32(kKeyWidth, &width);
    success = success && meta->findInt32(kKeyHeight, &height);
    CHECK(success);

    CODEC_LOGV("setVideoOutputFormat width=%ld, height=%ld", width, height);

    OMX_VIDEO_CODINGTYPE compressionFormat = OMX_VIDEO_CodingUnused;
    if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime)) {
        compressionFormat = OMX_VIDEO_CodingAVC;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG4, mime)) {
        compressionFormat = OMX_VIDEO_CodingMPEG4;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_H263, mime)) {
        compressionFormat = OMX_VIDEO_CodingH263;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VP8, mime)) {
        compressionFormat = OMX_VIDEO_CodingVP8;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VP9, mime)) {
        compressionFormat = OMX_VIDEO_CodingVP9;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG2, mime)) {
        compressionFormat = OMX_VIDEO_CodingMPEG2;
    } else if (!strncmp(mComponentName, "OMX.ffmpeg.", 11)) {
        if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_WMV, mime)) {
            status_t err = setWMVFormat(meta);
            if (err != OK) {
                CODEC_LOGE("setWMVFormat() failed (err = %d)", err);
                return err;
            }
            compressionFormat = OMX_VIDEO_CodingWMV;
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_RV, mime)) {
            status_t err = setRVFormat(meta);
            if (err != OK) {
                CODEC_LOGE("setRVFormat() failed (err = %d)", err);
                return err;
            }
            compressionFormat = OMX_VIDEO_CodingRV;
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_VC1, mime)) {
            compressionFormat = OMX_VIDEO_CodingVC1;
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_FLV1, mime)) {
            compressionFormat = OMX_VIDEO_CodingFLV1;
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_DIVX, mime)) {
            compressionFormat = OMX_VIDEO_CodingDIVX;
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_HEVC, mime)) {
            compressionFormat = OMX_VIDEO_CodingHEVC;
        } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_FFMPEG, mime)) {
            ALOGV("Setting the OMX_VIDEO_PARAM_FFMPEGTYPE params");
            status_t err = setFFmpegVideoFormat(meta);
            if (err != OK) {
                CODEC_LOGE("setFFmpegVideoFormat() failed (err = %d)", err);
                return err;
            }
            compressionFormat = OMX_VIDEO_CodingAutoDetect;
        }
#ifdef QCOM_HARDWARE
    } else if (!strncmp(mComponentName, "OMX.qcom.", 9)) {
        int32_t wmvVersion = 0;
        if (meta->findInt32(kKeyWMVVersion, &wmvVersion)) {
            if (wmvVersion == 1) {
                ALOGE("WMV2 is not supported");
                return ERROR_UNSUPPORTED;
            }
        }
        status_t err = ExtendedCodec::setVideoOutputFormat(mime, &compressionFormat);
        if(err != OK) {
            ALOGE("Not a supported video mime type: %s", mime);
        }
#endif
    }

    status_t err = setVideoPortFormatType(
            kPortIndexInput, compressionFormat, OMX_COLOR_FormatUnused);

    if (err != OK) {
        return err;
    }

#if 1
    {
        OMX_VIDEO_PARAM_PORTFORMATTYPE format;
        InitOMXParams(&format);
        format.nPortIndex = kPortIndexOutput;
        format.nIndex = 0;

        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamVideoPortFormat,
                &format, sizeof(format));
        CHECK_EQ(err, (status_t)OK);
        CHECK_EQ((int)format.eCompressionFormat, (int)OMX_VIDEO_CodingUnused);

#if 0
        CHECK(format.eColorFormat == OMX_COLOR_FormatYUV420Planar
               || format.eColorFormat == OMX_COLOR_FormatYUV420SemiPlanar
               || format.eColorFormat == OMX_COLOR_FormatCbYCrY
               || format.eColorFormat == OMX_TI_COLOR_FormatYUV420PackedSemiPlanar
               || format.eColorFormat == OMX_QCOM_COLOR_FormatYVU420SemiPlanar
               || format.eColorFormat == OMX_QCOM_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka
#ifdef USE_SAMSUNG_COLORFORMAT
               || format.eColorFormat == OMX_SEC_COLOR_FormatNV12TPhysicalAddress
               || format.eColorFormat == OMX_SEC_COLOR_FormatNV12Tiled
#endif
               );

#ifdef USE_SAMSUNG_COLORFORMAT
        if (!strncmp("OMX.SEC.", mComponentName, 8)) {
            if (mNativeWindow == NULL)
                format.eColorFormat = OMX_COLOR_FormatYUV420Planar;
            else
                format.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
        }
#endif

#endif

        int32_t colorFormat;
        if (meta->findInt32(kKeyColorFormat, &colorFormat)
                && colorFormat != OMX_COLOR_FormatUnused
                && colorFormat != format.eColorFormat) {

            while (OMX_ErrorNoMore != err) {
                format.nIndex++;
                err = mOMX->getParameter(
                        mNode, OMX_IndexParamVideoPortFormat,
                            &format, sizeof(format));
                if (format.eColorFormat == colorFormat) {
                    break;
                }
            }
            if (format.eColorFormat != colorFormat) {
                CODEC_LOGE("Color format %d is not supported", colorFormat);
                return ERROR_UNSUPPORTED;
            }
        }

        err = mOMX->setParameter(
                mNode, OMX_IndexParamVideoPortFormat,
                &format, sizeof(format));

        if (err != OK) {
            return err;
        }
    }
#endif

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexInput;

    OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    CHECK_EQ(err, (status_t)OK);

#if 1
    // XXX Need a (much) better heuristic to compute input buffer sizes.
#ifdef USE_SAMSUNG_COLORFORMAT
    const size_t X = 64 * 8 * 1024;
#else
    const size_t X = 64 * 1024;
#endif
    if (def.nBufferSize < X) {
        def.nBufferSize = X;
    }
#endif

    CHECK_EQ((int)def.eDomain, (int)OMX_PortDomainVideo);

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;

    video_def->eCompressionFormat = compressionFormat;
    video_def->eColorFormat = OMX_COLOR_FormatUnused;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    ////////////////////////////////////////////////////////////////////////////

    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);
    CHECK_EQ((int)def.eDomain, (int)OMX_PortDomainVideo);

#if 0
    def.nBufferSize =
        (((width + 15) & -16) * ((height + 15) & -16) * 3) / 2;  // YUV420
#endif

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    return err;
}

OMXCodec::OMXCodec(
        const sp<IOMX> &omx, IOMX::node_id node,
        uint32_t quirks, uint32_t flags,
        bool isEncoder,
        const char *mime,
        const char *componentName,
        const sp<MediaSource> &source,
        const sp<ANativeWindow> &nativeWindow)
    : mOMX(omx),
      mOMXLivesLocally(omx->livesLocally(node, getpid())),
      mNode(node),
      mQuirks(quirks),
      mFlags(flags),
      mIsEncoder(isEncoder),
      mIsVideo(!strncasecmp("video/", mime, 6)),
      mMIME(strdup(mime)),
      mComponentName(strdup(componentName)),
      mCodecSpecificDataIndex(0),
      mState(LOADED),
      mInitialBufferSubmit(true),
      mSignalledEOS(false),
      mNoMoreOutputData(false),
      mOutputPortSettingsHaveChanged(false),
      mSeekTimeUs(-1),
      mSeekMode(ReadOptions::SEEK_CLOSEST_SYNC),
      mTargetTimeUs(-1),
      mOutputPortSettingsChangedPending(false),
      mSkipCutBuffer(NULL),
      mLeftOverBuffer(NULL),
      mPaused(false),
      mNativeWindow(
              (!strncmp(componentName, "OMX.google.", 11)
              || !strncmp(componentName, "OMX.ffmpeg.", 11))
                        ? NULL : nativeWindow),
#ifdef QCOM_HARDWARE
      mNumBFrames(0),
#endif
      mInSmoothStreamingMode(false),
      mOutputCropChanged(false),
      mSignalledReadTryAgain(false),
      mReturnedRetry(false),
      mLastSeekTimeUs(-1),
      mLastSeekMode(ReadOptions::SEEK_CLOSEST) {
    mPortStatus[kPortIndexInput] = ENABLING;
    mPortStatus[kPortIndexOutput] = ENABLING;

    setComponentRole();
#ifdef ENABLE_AV_ENHANCEMENTS
    // cascade a prefetching-source for video playback excluding secure and
    // thumbnail modes
    if (mIsVideo && !mIsEncoder && !(mFlags & kUseSecureInputBuffers) &&
            (mNativeWindow != NULL) && PrefetchSource::isPrefetchEnabled()) {
        ALOGI("Creating Prefetching source for video");
        mSource = new PrefetchSource(source,
                PrefetchSource::MODE_FRAME_BY_FRAME, "VideoPrefetch");
    } else
#endif
        mSource = source;
}

// static
void OMXCodec::setComponentRole(
        const sp<IOMX> &omx, IOMX::node_id node, bool isEncoder,
        const char *mime) {
    struct MimeToRole {
        const char *mime;
        const char *decoderRole;
        const char *encoderRole;
    };

    static const MimeToRole kMimeToRole[] = {
        { MEDIA_MIMETYPE_AUDIO_MPEG,
            "audio_decoder.mp3", "audio_encoder.mp3" },
        { MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_I,
            "audio_decoder.mp1", "audio_encoder.mp1" },
        { MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II,
            "audio_decoder.mp2", "audio_encoder.mp2" },
        { MEDIA_MIMETYPE_AUDIO_AMR_NB,
            "audio_decoder.amrnb", "audio_encoder.amrnb" },
        { MEDIA_MIMETYPE_AUDIO_AMR_WB,
            "audio_decoder.amrwb", "audio_encoder.amrwb" },
#ifdef ENABLE_AV_ENHANCEMENTS
        { MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS,
            "audio_decoder.amrwbplus", "audio_encoder.amrwbplus" },
#endif
        { MEDIA_MIMETYPE_AUDIO_AAC,
            "audio_decoder.aac", "audio_encoder.aac" },
        { MEDIA_MIMETYPE_AUDIO_VORBIS,
            "audio_decoder.vorbis", "audio_encoder.vorbis" },
        { MEDIA_MIMETYPE_AUDIO_G711_MLAW,
            "audio_decoder.g711mlaw", "audio_encoder.g711mlaw" },
        { MEDIA_MIMETYPE_AUDIO_G711_ALAW,
            "audio_decoder.g711alaw", "audio_encoder.g711alaw" },
#ifdef ENABLE_AV_ENHANCEMENTS
        { MEDIA_MIMETYPE_AUDIO_EVRC,
            "audio_decoder.evrchw", "audio_encoder.evrc" },
        { MEDIA_MIMETYPE_AUDIO_QCELP,
            "audio_decoder,qcelp13Hw", "audio_encoder.qcelp13" },
#endif
        { MEDIA_MIMETYPE_VIDEO_AVC,
            "video_decoder.avc", "video_encoder.avc" },
        { MEDIA_MIMETYPE_VIDEO_MPEG4,
            "video_decoder.mpeg4", "video_encoder.mpeg4" },
        { MEDIA_MIMETYPE_VIDEO_H263,
            "video_decoder.h263", "video_encoder.h263" },
        { MEDIA_MIMETYPE_VIDEO_VP8,
            "video_decoder.vp8", "video_encoder.vp8" },
        { MEDIA_MIMETYPE_VIDEO_VP9,
            "video_decoder.vp9", "video_encoder.vp9" },
        { MEDIA_MIMETYPE_AUDIO_RAW,
            "audio_decoder.raw", "audio_encoder.raw" },
        { MEDIA_MIMETYPE_AUDIO_FLAC,
            "audio_decoder.flac", "audio_encoder.flac" },
        { MEDIA_MIMETYPE_AUDIO_MSGSM,
            "audio_decoder.gsm", "audio_encoder.gsm" },
    };

    static const size_t kNumMimeToRole =
        sizeof(kMimeToRole) / sizeof(kMimeToRole[0]);

    size_t i;
    for (i = 0; i < kNumMimeToRole; ++i) {
        if (!strcasecmp(mime, kMimeToRole[i].mime)) {
            break;
        }
    }

    if (i == kNumMimeToRole) {
#ifdef QCOM_HARDWARE
        ExtendedCodec::setSupportedRole(omx, node, isEncoder, mime);
#endif
        return;
    }

    const char *role =
        isEncoder ? kMimeToRole[i].encoderRole
                  : kMimeToRole[i].decoderRole;

    if (role != NULL) {
        OMX_PARAM_COMPONENTROLETYPE roleParams;
        InitOMXParams(&roleParams);

        strncpy((char *)roleParams.cRole,
                role, OMX_MAX_STRINGNAME_SIZE - 1);

        roleParams.cRole[OMX_MAX_STRINGNAME_SIZE - 1] = '\0';

        status_t err = omx->setParameter(
                node, OMX_IndexParamStandardComponentRole,
                &roleParams, sizeof(roleParams));

        if (err != OK) {
            ALOGW("Failed to set standard component role '%s'.", role);
        }
    }
}

void OMXCodec::setComponentRole() {
    setComponentRole(mOMX, mNode, mIsEncoder, mMIME);
}

OMXCodec::~OMXCodec() {
    mSource.clear();

    CHECK(mState == LOADED || mState == ERROR || mState == LOADED_TO_IDLE);

    status_t err = mOMX->freeNode(mNode);
    CHECK_EQ(err, (status_t)OK);

    mNode = NULL;

    releaseMediaBuffersOn(kPortIndexOutput);
    releaseMediaBuffersOn(kPortIndexInput);

    setState(DEAD);

    clearCodecSpecificData();

    free(mComponentName);
    mComponentName = NULL;

    free(mMIME);
    mMIME = NULL;
}

status_t OMXCodec::init() {
    // mLock is held.

    CHECK_EQ((int)mState, (int)LOADED);

    status_t err;
    if (!(mQuirks & kRequiresLoadedToIdleAfterAllocation)) {
        err = mOMX->sendCommand(mNode, OMX_CommandStateSet, OMX_StateIdle);
        CHECK_EQ(err, (status_t)OK);
        setState(LOADED_TO_IDLE);
    }

    err = allocateBuffers();
    if (err != (status_t)OK) {
        CODEC_LOGE("Allocate Buffer failed - error = %d", err);
        setState(ERROR);
        return err;
    }

    if (mQuirks & kRequiresLoadedToIdleAfterAllocation) {
        err = mOMX->sendCommand(mNode, OMX_CommandStateSet, OMX_StateIdle);
        CHECK_EQ(err, (status_t)OK);

        setState(LOADED_TO_IDLE);
    }

    while (mState != EXECUTING && mState != ERROR) {
        mAsyncCompletion.wait(mLock);
    }

    // If the native window is valid, we need to do the extra work of
    // cancelling buffers back.
    if (mState == ERROR) {
        flushBuffersOnError();
    }

    return mState == ERROR ? UNKNOWN_ERROR : OK;
}

// static
bool OMXCodec::isIntermediateState(State state) {
    return state == LOADED_TO_IDLE
        || state == IDLE_TO_EXECUTING
        || state == EXECUTING_TO_IDLE
        || state == PAUSING
        || state == FLUSHING
        || state == IDLE_TO_LOADED
        || state == RECONFIGURING;
}

status_t OMXCodec::allocateBuffers() {
    status_t err = allocateBuffersOnPort(kPortIndexInput);

    if (err != OK) {
        return err;
    }

    return allocateBuffersOnPort(kPortIndexOutput);
}

status_t OMXCodec::allocateBuffersOnPort(OMX_U32 portIndex) {
    if (mNativeWindow != NULL && portIndex == kPortIndexOutput) {
        return allocateOutputBuffersFromNativeWindow();
    }

    if ((mFlags & kEnableGrallocUsageProtected) && portIndex == kPortIndexOutput) {
        ALOGE("protected output buffers must be stent to an ANativeWindow");
        return PERMISSION_DENIED;
    }

    status_t err = OK;
    if ((mFlags & kStoreMetaDataInVideoBuffers)
            && portIndex == kPortIndexInput) {
        err = mOMX->storeMetaDataInBuffers(mNode, kPortIndexInput, OMX_TRUE);
        if (err != OK) {
            ALOGE("Storing meta data in video buffers is not supported");
            return err;
        }
    }

#if defined(ENABLE_AV_ENHANCEMENTS) || defined(ENABLE_OFFLOAD_ENHANCEMENTS)
    if (!mIsVideo && portIndex == kPortIndexOutput &&
            !strncmp(mComponentName, "OMX.ffmpeg.", 11)) {
        ExtendedCodec::updatePcmOutputFormat(mOutputFormat, mOMX, mNode, NULL);
    }
#endif

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    CODEC_LOGI("allocating %lu buffers of size %lu on %s port",
            def.nBufferCountActual, def.nBufferSize,
            portIndex == kPortIndexInput ? "input" : "output");

    size_t totalSize = def.nBufferCountActual * def.nBufferSize;
    mDealer[portIndex] = new MemoryDealer(totalSize, "OMXCodec");

    for (OMX_U32 i = 0; i < def.nBufferCountActual; ++i) {
        sp<IMemory> mem = mDealer[portIndex]->allocate(def.nBufferSize);
        CHECK(mem.get() != NULL);

        BufferInfo info;
        info.mData = NULL;
        info.mSize = def.nBufferSize;

        IOMX::buffer_id buffer;
        if (portIndex == kPortIndexInput
                && ((mQuirks & kRequiresAllocateBufferOnInputPorts)
                    || (mFlags & kUseSecureInputBuffers))) {
            if (mOMXLivesLocally) {
                mem.clear();

                err = mOMX->allocateBuffer(
                        mNode, portIndex, def.nBufferSize, &buffer,
                        &info.mData);
            } else {
                err = mOMX->allocateBufferWithBackup(
                        mNode, portIndex, mem, &buffer);
            }
        } else if (portIndex == kPortIndexOutput
                && (mQuirks & kRequiresAllocateBufferOnOutputPorts)) {
            if (mOMXLivesLocally) {
                mem.clear();

                err = mOMX->allocateBuffer(
                        mNode, portIndex, def.nBufferSize, &buffer,
                        &info.mData);
            } else {
                err = mOMX->allocateBufferWithBackup(
                        mNode, portIndex, mem, &buffer);
            }
        } else {
            err = mOMX->useBuffer(mNode, portIndex, mem, &buffer);
        }

        if (err != OK) {
            ALOGE("allocate_buffer_with_backup failed");
            return err;
        }

        if (mem != NULL) {
            info.mData = mem->pointer();
        }

        info.mBuffer = buffer;
        info.mStatus = OWNED_BY_US;
        info.mMem = mem;
        info.mMediaBuffer = NULL;
        info.mOutputCropChanged = false;

        if (portIndex == kPortIndexOutput) {
            if (!(mOMXLivesLocally
                        && (mQuirks & kRequiresAllocateBufferOnOutputPorts)
                        && (mQuirks & kDefersOutputBufferAllocation))) {
                // If the node does not fill in the buffer ptr at this time,
                // we will defer creating the MediaBuffer until receiving
                // the first FILL_BUFFER_DONE notification instead.
                info.mMediaBuffer = new MediaBuffer(info.mData, info.mSize);
                info.mMediaBuffer->setObserver(this);
            }
        }

        mPortBuffers[portIndex].push(info);

        CODEC_LOGI("allocated buffer %p on %s port", buffer,
             portIndex == kPortIndexInput ? "input" : "output");
    }

    if (portIndex == kPortIndexOutput) {

        sp<MetaData> meta = mSource->getFormat();
        int32_t delay = 0;
        if (!meta->findInt32(kKeyEncoderDelay, &delay)) {
            delay = 0;
        }
        int32_t padding = 0;
        if (!meta->findInt32(kKeyEncoderPadding, &padding)) {
            padding = 0;
        }
        int32_t numchannels = 0;
        if (delay + padding) {
            if (mOutputFormat->findInt32(kKeyChannelCount, &numchannels)) {
                size_t frameSize = numchannels * sizeof(int16_t);
                if (mSkipCutBuffer != NULL) {
                    size_t prevbuffersize = mSkipCutBuffer->size();
                    if (prevbuffersize != 0) {
                        ALOGW("Replacing SkipCutBuffer holding %d bytes", prevbuffersize);
                    }
                }
                mSkipCutBuffer = new SkipCutBuffer(delay * frameSize, padding * frameSize);
            }
        }
    }

    // dumpPortStatus(portIndex);

    if (portIndex == kPortIndexInput && (mFlags & kUseSecureInputBuffers)) {
        Vector<MediaBuffer *> buffers;
        for (size_t i = 0; i < def.nBufferCountActual; ++i) {
            const BufferInfo &info = mPortBuffers[kPortIndexInput].itemAt(i);

            MediaBuffer *mbuf = new MediaBuffer(info.mData, info.mSize);
            buffers.push(mbuf);
        }

        status_t err = mSource->setBuffers(buffers);

        if (err != OK) {
            for (size_t i = 0; i < def.nBufferCountActual; ++i) {
                buffers.editItemAt(i)->release();
            }
            buffers.clear();

            CODEC_LOGE(
                    "Codec requested to use secure input buffers but "
                    "upstream source didn't support that.");

            return err;
        }
    }

    return OK;
}

status_t OMXCodec::applyRotation() {
    sp<MetaData> meta = mSource->getFormat();

    int32_t rotationDegrees;
    if (!meta->findInt32(kKeyRotation, &rotationDegrees)) {
        rotationDegrees = 0;
    }

    uint32_t transform;
    switch (rotationDegrees) {
        case 0: transform = 0; break;
        case 90: transform = HAL_TRANSFORM_ROT_90; break;
        case 180: transform = HAL_TRANSFORM_ROT_180; break;
        case 270: transform = HAL_TRANSFORM_ROT_270; break;
        default: transform = 0; break;
    }

    status_t err = OK;

    if (transform) {
        err = native_window_set_buffers_transform(
                mNativeWindow.get(), transform);
        ALOGE("native_window_set_buffers_transform failed: %s (%d)",
                strerror(-err), -err);
    }

    return err;
}

status_t OMXCodec::allocateOutputBuffersFromNativeWindow() {
    // Get the number of buffers needed.
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    if (err != OK) {
        CODEC_LOGE("getParameter failed: %d", err);
        return err;
    }

#ifndef USE_SAMSUNG_COLORFORMAT
    err = native_window_set_buffers_geometry(
            mNativeWindow.get(),
            def.format.video.nFrameWidth,
            def.format.video.nFrameHeight,
            def.format.video.eColorFormat);
#else
    OMX_COLOR_FORMATTYPE eColorFormat;

    switch (def.format.video.eColorFormat) {
    case OMX_SEC_COLOR_FormatNV12TPhysicalAddress:
        eColorFormat = (OMX_COLOR_FORMATTYPE)HAL_PIXEL_FORMAT_CUSTOM_YCbCr_420_SP_TILED;
        break;
    case OMX_COLOR_FormatYUV420SemiPlanar:
        eColorFormat = (OMX_COLOR_FORMATTYPE)HAL_PIXEL_FORMAT_YCbCr_420_SP;
        break;
    case OMX_COLOR_FormatYUV420Planar:
    default:
        eColorFormat = (OMX_COLOR_FORMATTYPE)HAL_PIXEL_FORMAT_YCbCr_420_P;
        break;
    }

    err = native_window_set_buffers_geometry(
            mNativeWindow.get(),
            def.format.video.nFrameWidth,
            def.format.video.nFrameHeight,
            eColorFormat);
#endif

    if (err != 0) {
        ALOGE("native_window_set_buffers_geometry failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    err = applyRotation();
    if (err != OK) {
        return err;
    }

    // Set up the native window.
    OMX_U32 usage = 0;
    err = mOMX->getGraphicBufferUsage(mNode, kPortIndexOutput, &usage);
    if (err != 0) {
        ALOGW("querying usage flags from OMX IL component failed: %d", err);
        // XXX: Currently this error is logged, but not fatal.
        usage = 0;
    }
    if (mFlags & kEnableGrallocUsageProtected) {
        usage |= GRALLOC_USAGE_PROTECTED;
    }

    // Make sure to check whether either Stagefright or the video decoder
    // requested protected buffers.
    if (usage & GRALLOC_USAGE_PROTECTED) {
        // Verify that the ANativeWindow sends images directly to
        // SurfaceFlinger.
        int queuesToNativeWindow = 0;
        err = mNativeWindow->query(
                mNativeWindow.get(), NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER,
                &queuesToNativeWindow);
        if (err != 0) {
            ALOGE("error authenticating native window: %d", err);
            return err;
        }
        if (queuesToNativeWindow != 1) {
            ALOGE("native window could not be authenticated");
            return PERMISSION_DENIED;
        }
    }

    ALOGV("native_window_set_usage usage=0x%lx", usage);

    err = native_window_set_usage(
            mNativeWindow.get(), usage | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP);

    if (err != 0) {
        ALOGE("native_window_set_usage failed: %s (%d)", strerror(-err), -err);
        return err;
    }

    int minUndequeuedBufs = 0;
    err = mNativeWindow->query(mNativeWindow.get(),
            NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minUndequeuedBufs);
    if (err != 0) {
        ALOGE("NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS query failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    // XXX: Is this the right logic to use?  It's not clear to me what the OMX
    // buffer counts refer to - how do they account for the renderer holding on
    // to buffers?
    if (def.nBufferCountActual < def.nBufferCountMin + minUndequeuedBufs) {
        OMX_U32 newBufferCount = def.nBufferCountMin + minUndequeuedBufs;
        def.nBufferCountActual = newBufferCount;
        err = mOMX->setParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
        if (err != OK) {
            CODEC_LOGE("setting nBufferCountActual to %lu failed: %d",
                    newBufferCount, err);
            return err;
        }
    }

    err = native_window_set_buffer_count(
            mNativeWindow.get(), def.nBufferCountActual);
    if (err != 0) {
        ALOGE("native_window_set_buffer_count failed: %s (%d)", strerror(-err),
                -err);
        return err;
    }

    CODEC_LOGV("allocating %lu buffers from a native window of size %lu on "
            "output port", def.nBufferCountActual, def.nBufferSize);

    // Dequeue buffers and send them to OMX
    for (OMX_U32 i = 0; i < def.nBufferCountActual; i++) {
        ANativeWindowBuffer* buf;
        err = native_window_dequeue_buffer_and_wait(mNativeWindow.get(), &buf);
        if (err != 0) {
            ALOGE("dequeueBuffer failed: %s (%d)", strerror(-err), -err);
            break;
        }

        sp<GraphicBuffer> graphicBuffer(new GraphicBuffer(buf, false));
        BufferInfo info;
        info.mData = NULL;
        info.mSize = def.nBufferSize;
        info.mStatus = OWNED_BY_US;
        info.mMem = NULL;
        info.mMediaBuffer = new MediaBuffer(graphicBuffer);
        info.mMediaBuffer->setObserver(this);

        IOMX::buffer_id bufferId;
        err = mOMX->useGraphicBuffer(mNode, kPortIndexOutput, graphicBuffer,
                &bufferId);
        if (err != 0) {
            CODEC_LOGE("registering GraphicBuffer with OMX IL component "
                    "failed: %d", err);
            info.mMediaBuffer->setObserver(NULL);
            info.mMediaBuffer->release();
            break;
        }

        mPortBuffers[kPortIndexOutput].push(info);
        mPortBuffers[kPortIndexOutput].editItemAt(i).mBuffer = bufferId;

        CODEC_LOGV("registered graphic buffer with ID %p (pointer = %p)",
                bufferId, graphicBuffer.get());
    }

    OMX_U32 cancelStart;
    OMX_U32 cancelEnd;
    if (err != 0) {
        // If an error occurred while dequeuing we need to cancel any buffers
        // that were dequeued.
        cancelStart = 0;
        cancelEnd = mPortBuffers[kPortIndexOutput].size();
    } else {
        // Return the last two buffers to the native window.
        cancelStart = def.nBufferCountActual - minUndequeuedBufs;
        cancelEnd = def.nBufferCountActual;
    }


    if (err != 0 &&
        ((mState == LOADED) || (mState == LOADED_TO_IDLE))) {
        freeBuffersOnPort(kPortIndexOutput);
    } else {
        for (OMX_U32 i = cancelStart; i < cancelEnd; i++) {
            BufferInfo *info = &mPortBuffers[kPortIndexOutput].editItemAt(i);
            cancelBufferToNativeWindow(info);
        }
    }

    return err;
}

status_t OMXCodec::cancelBufferToNativeWindow(BufferInfo *info) {
    CHECK_EQ((int)info->mStatus, (int)OWNED_BY_US);
    CODEC_LOGV("Calling cancelBuffer on buffer %p", info->mBuffer);
    int err = mNativeWindow->cancelBuffer(
        mNativeWindow.get(), info->mMediaBuffer->graphicBuffer().get(), -1);
    if (err != 0) {
      CODEC_LOGE("cancelBuffer failed w/ error 0x%08x", err);

      setState(ERROR);
      return err;
    }
    info->mStatus = OWNED_BY_NATIVE_WINDOW;
    return OK;
}

OMXCodec::BufferInfo* OMXCodec::dequeueBufferFromNativeWindow() {
    // Dequeue the next buffer from the native window.
    ANativeWindowBuffer* buf;
    int fenceFd = -1;

    // dequeue-and-wait can block. relinquish mLock to
    // let other thread (CallbackDispatcher) do some useful work
    mLock.unlock();
    int err = native_window_dequeue_buffer_and_wait(mNativeWindow.get(), &buf);
    mLock.lock();
    if (err != 0) {
      CODEC_LOGE("dequeueBuffer failed w/ error 0x%08x", err);

      setState(ERROR);
      return 0;
    }

    // Determine which buffer we just dequeued.
    Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexOutput];
    BufferInfo *bufInfo = 0;
    for (size_t i = 0; i < buffers->size(); i++) {
      sp<GraphicBuffer> graphicBuffer = buffers->itemAt(i).
          mMediaBuffer->graphicBuffer();
      if (graphicBuffer->handle == buf->handle) {
        bufInfo = &buffers->editItemAt(i);
        break;
      }
    }

    if (bufInfo == 0) {
        CODEC_LOGE("dequeued unrecognized buffer: %p", buf);

        setState(ERROR);
        return 0;
    }

    // The native window no longer owns the buffer.
    CHECK_EQ((int)bufInfo->mStatus, (int)OWNED_BY_NATIVE_WINDOW);
    bufInfo->mStatus = OWNED_BY_US;

    return bufInfo;
}

status_t OMXCodec::pushBlankBuffersToNativeWindow() {
    status_t err = NO_ERROR;
    ANativeWindowBuffer* anb = NULL;
    int numBufs = 0;
    int minUndequeuedBufs = 0;

    // We need to reconnect to the ANativeWindow as a CPU client to ensure that
    // no frames get dropped by SurfaceFlinger assuming that these are video
    // frames.
    err = native_window_api_disconnect(mNativeWindow.get(),
            NATIVE_WINDOW_API_MEDIA);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank frames: api_disconnect failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    err = native_window_api_connect(mNativeWindow.get(),
            NATIVE_WINDOW_API_CPU);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank frames: api_connect failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    err = native_window_set_buffers_geometry(mNativeWindow.get(), 1, 1,
            HAL_PIXEL_FORMAT_RGBX_8888);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank frames: set_buffers_geometry failed: %s (%d)",
                strerror(-err), -err);
        goto error;
    }

    err = native_window_set_usage(mNativeWindow.get(),
            GRALLOC_USAGE_SW_WRITE_OFTEN);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank frames: set_usage failed: %s (%d)",
                strerror(-err), -err);
        goto error;
    }

    err = native_window_set_scaling_mode(mNativeWindow.get(),
            NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
    if (err != OK) {
        ALOGE("error pushing blank frames: set_scaling_mode failed: %s (%d)",
                strerror(-err), -err);
        goto error;
    }

    err = mNativeWindow->query(mNativeWindow.get(),
            NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minUndequeuedBufs);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank frames: MIN_UNDEQUEUED_BUFFERS query "
                "failed: %s (%d)", strerror(-err), -err);
        goto error;
    }

    numBufs = minUndequeuedBufs + 1;
    err = native_window_set_buffer_count(mNativeWindow.get(), numBufs);
    if (err != NO_ERROR) {
        ALOGE("error pushing blank frames: set_buffer_count failed: %s (%d)",
                strerror(-err), -err);
        goto error;
    }

    // We  push numBufs + 1 buffers to ensure that we've drawn into the same
    // buffer twice.  This should guarantee that the buffer has been displayed
    // on the screen and then been replaced, so an previous video frames are
    // guaranteed NOT to be currently displayed.
    for (int i = 0; i < numBufs + 1; i++) {
        int fenceFd = -1;
        err = native_window_dequeue_buffer_and_wait(mNativeWindow.get(), &anb);
        if (err != NO_ERROR) {
            ALOGE("error pushing blank frames: dequeueBuffer failed: %s (%d)",
                    strerror(-err), -err);
            goto error;
        }

        sp<GraphicBuffer> buf(new GraphicBuffer(anb, false));

        // Fill the buffer with the a 1x1 checkerboard pattern ;)
        uint32_t* img = NULL;
        err = buf->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, (void**)(&img));
        if (err != NO_ERROR) {
            ALOGE("error pushing blank frames: lock failed: %s (%d)",
                    strerror(-err), -err);
            goto error;
        }

        *img = 0;

        err = buf->unlock();
        if (err != NO_ERROR) {
            ALOGE("error pushing blank frames: unlock failed: %s (%d)",
                    strerror(-err), -err);
            goto error;
        }

        err = mNativeWindow->queueBuffer(mNativeWindow.get(),
                buf->getNativeBuffer(), -1);
        if (err != NO_ERROR) {
            ALOGE("error pushing blank frames: queueBuffer failed: %s (%d)",
                    strerror(-err), -err);
            goto error;
        }

        anb = NULL;
    }

error:

    if (err != NO_ERROR) {
        // Clean up after an error.
        if (anb != NULL) {
            mNativeWindow->cancelBuffer(mNativeWindow.get(), anb, -1);
        }

        native_window_api_disconnect(mNativeWindow.get(),
                NATIVE_WINDOW_API_CPU);
        native_window_api_connect(mNativeWindow.get(),
                NATIVE_WINDOW_API_MEDIA);

        return err;
    } else {
        // Clean up after success.
        err = native_window_api_disconnect(mNativeWindow.get(),
                NATIVE_WINDOW_API_CPU);
        if (err != NO_ERROR) {
            ALOGE("error pushing blank frames: api_disconnect failed: %s (%d)",
                    strerror(-err), -err);
            return err;
        }

        err = native_window_api_connect(mNativeWindow.get(),
                NATIVE_WINDOW_API_MEDIA);
        if (err != NO_ERROR) {
            ALOGE("error pushing blank frames: api_connect failed: %s (%d)",
                    strerror(-err), -err);
            return err;
        }

        return NO_ERROR;
    }
}

int64_t OMXCodec::getDecodingTimeUs() {
    CHECK(mIsEncoder && mIsVideo);

    if (mDecodingTimeList.empty()) {
        CHECK(mSignalledEOS || mNoMoreOutputData);
        // No corresponding input frame available.
        // This could happen when EOS is reached.
        return 0;
    }

    List<int64_t>::iterator it = mDecodingTimeList.begin();
    int64_t timeUs = *it;
    mDecodingTimeList.erase(it);
    return timeUs;
}

void OMXCodec::on_message(const omx_message &msg) {
    if (mState == ERROR) {
        /*
         * only drop EVENT messages, EBD and FBD are still
         * processed for bookkeeping purposes
         */
        if (msg.type == omx_message::EVENT) {
            ALOGW("Dropping OMX EVENT message - we're in ERROR state.");
            return;
        }
    }

    switch (msg.type) {
        case omx_message::EVENT:
        {
            onEvent(
                 msg.u.event_data.event, msg.u.event_data.data1,
                 msg.u.event_data.data2);

            break;
        }

        case omx_message::EMPTY_BUFFER_DONE:
        {
            ATRACE_BEGIN("EMPTY_BUFFER_DONE");
            IOMX::buffer_id buffer = msg.u.extended_buffer_data.buffer;

            CODEC_LOGV("EMPTY_BUFFER_DONE(buffer: %p)", buffer);

            Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexInput];
            size_t i = 0;
            while (i < buffers->size() && (*buffers)[i].mBuffer != buffer) {
                ++i;
            }

            CHECK(i < buffers->size());
            if ((*buffers)[i].mStatus != OWNED_BY_COMPONENT) {
                ALOGW("We already own input buffer %p, yet received "
                     "an EMPTY_BUFFER_DONE.", buffer);
            }

            BufferInfo* info = &buffers->editItemAt(i);
            info->mStatus = OWNED_BY_US;

            // Buffer could not be released until empty buffer done is called.
            if (info->mMediaBuffer != NULL) {
#ifdef OMAP_ENHANCEMENT
                if (mIsEncoder &&
                    (mQuirks & kAvoidMemcopyInputRecordingFrames)) {
                    // If zero-copy mode is enabled this will send the
                    // input buffer back to the upstream source.
                    restorePatchedDataPointer(info);
                }
#endif
                info->mMediaBuffer->release();
                info->mMediaBuffer = NULL;
            }

            if (mPortStatus[kPortIndexInput] == DISABLING) {
                CODEC_LOGV("Port is disabled, freeing buffer %p", buffer);

                status_t err = freeBuffer(kPortIndexInput, i);
                CHECK_EQ(err, (status_t)OK);
            } else if (mState != ERROR
                    && mPortStatus[kPortIndexInput] != SHUTTING_DOWN) {
                CHECK_EQ((int)mPortStatus[kPortIndexInput], (int)ENABLED);

                if (mFlags & kUseSecureInputBuffers) {
                    drainAnyInputBuffer();
                } else {
                    drainInputBuffer(&buffers->editItemAt(i));
                }
            }
            ATRACE_END();
            break;
        }

        case omx_message::FILL_BUFFER_DONE:
        {
            ATRACE_BEGIN("FILL_BUFFER_DONE");
            IOMX::buffer_id buffer = msg.u.extended_buffer_data.buffer;
            OMX_U32 flags = msg.u.extended_buffer_data.flags;

            CODEC_LOGV("FILL_BUFFER_DONE(buffer: %p, size: %ld, flags: 0x%08lx, timestamp: %lld us (%.2f secs))",
                 buffer,
                 msg.u.extended_buffer_data.range_length,
                 flags,
                 msg.u.extended_buffer_data.timestamp,
                 msg.u.extended_buffer_data.timestamp / 1E6);

            Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexOutput];
            size_t i = 0;
            while (i < buffers->size() && (*buffers)[i].mBuffer != buffer) {
                ++i;
            }

            CHECK(i < buffers->size());
            BufferInfo *info = &buffers->editItemAt(i);

            if (info->mStatus != OWNED_BY_COMPONENT) {
                ALOGW("We already own output buffer %p, yet received "
                     "a FILL_BUFFER_DONE.", buffer);
            }

            info->mStatus = OWNED_BY_US;

            if (mPortStatus[kPortIndexOutput] == DISABLING) {
                CODEC_LOGV("Port is disabled, freeing buffer %p", buffer);

                status_t err = freeBuffer(kPortIndexOutput, i);
                CHECK_EQ(err, (status_t)OK);

#if 0
            } else if (mPortStatus[kPortIndexOutput] == ENABLED
                       && (flags & OMX_BUFFERFLAG_EOS)) {
                CODEC_LOGV("No more output data.");
                mNoMoreOutputData = true;
                mBufferFilled.signal();
#endif
            } else if (mPortStatus[kPortIndexOutput] != SHUTTING_DOWN) {
                CHECK_EQ((int)mPortStatus[kPortIndexOutput], (int)ENABLED);

                if (info->mMediaBuffer == NULL) {
                    CHECK(mOMXLivesLocally);
                    CHECK(mQuirks & kRequiresAllocateBufferOnOutputPorts);
                    CHECK(mQuirks & kDefersOutputBufferAllocation);

                    // The qcom video decoders on Nexus don't actually allocate
                    // output buffer memory on a call to OMX_AllocateBuffer
                    // the "pBuffer" member of the OMX_BUFFERHEADERTYPE
                    // structure is only filled in later.

                    info->mMediaBuffer = new MediaBuffer(
                            msg.u.extended_buffer_data.data_ptr,
                            info->mSize);
                    info->mMediaBuffer->setObserver(this);
                }

                MediaBuffer *buffer = info->mMediaBuffer;
                bool isGraphicBuffer = buffer->graphicBuffer() != NULL;

                if (!isGraphicBuffer
                    && msg.u.extended_buffer_data.range_offset
                        + msg.u.extended_buffer_data.range_length
                            > buffer->size()) {
                    CODEC_LOGE(
                            "Codec lied about its buffer size requirements, "
                            "sending a buffer larger than the originally "
                            "advertised size in FILL_BUFFER_DONE!");
                }
                buffer->set_range(
                        msg.u.extended_buffer_data.range_offset,
                        msg.u.extended_buffer_data.range_length);

                buffer->meta_data()->clear();

                buffer->meta_data()->setInt64(
                        kKeyTime, msg.u.extended_buffer_data.timestamp);

                if (msg.u.extended_buffer_data.flags & OMX_BUFFERFLAG_SYNCFRAME) {
                    buffer->meta_data()->setInt32(kKeyIsSyncFrame, true);
                }
                bool isCodecSpecific = false;
                if (msg.u.extended_buffer_data.flags & OMX_BUFFERFLAG_CODECCONFIG) {
                    buffer->meta_data()->setInt32(kKeyIsCodecConfig, true);
                    isCodecSpecific = true;
                }

                if (isGraphicBuffer || mQuirks & kOutputBuffersAreUnreadable) {
                    buffer->meta_data()->setInt32(kKeyIsUnreadable, true);
                }

                buffer->meta_data()->setPointer(
                        kKeyPlatformPrivate,
                        msg.u.extended_buffer_data.platform_private);

                buffer->meta_data()->setPointer(
                        kKeyBufferID,
                        msg.u.extended_buffer_data.buffer);

                if (msg.u.extended_buffer_data.flags & OMX_BUFFERFLAG_EOS) {
                    CODEC_LOGV("No more output data.");
                    mNoMoreOutputData = true;
                }

                if (mIsEncoder && mIsVideo) {
                    int64_t decodingTimeUs = isCodecSpecific? 0: getDecodingTimeUs();
                    buffer->meta_data()->setInt64(kKeyDecodingTime, decodingTimeUs);
                }

                if (mTargetTimeUs >= 0) {
                    CHECK(msg.u.extended_buffer_data.timestamp <= mTargetTimeUs);

                    if (msg.u.extended_buffer_data.timestamp < mTargetTimeUs) {
                        CODEC_LOGV(
                                "skipping output buffer at timestamp %lld us",
                                msg.u.extended_buffer_data.timestamp);

                        fillOutputBuffer(info);
                        ATRACE_END();
                        break;
                    }

                    CODEC_LOGV(
                            "returning output buffer at target timestamp "
                            "%lld us",
                            msg.u.extended_buffer_data.timestamp);

                    mTargetTimeUs = -1;
                }

                if (mOutputCropChanged) {
                    mOutputCropChanged = false;
                    info->mOutputCropChanged = true;
                }
                mFilledBuffers.push_back(i);
                mBufferFilled.signal();
                if (!strncasecmp(mMIME, "video/", 6)) {
                    ATRACE_INT("Output buffers with OMXCodec", mFilledBuffers.size());
                    ATRACE_INT("Output Buffers with OMX client",
                            countBuffersWeOwn(mPortBuffers[kPortIndexOutput]));
                }
            }

            ATRACE_END();
            break;
        }

        default:
        {
            CHECK(!"should not be here.");
            break;
        }
    }
}

// Has the format changed in any way that the client would have to be aware of?
static bool formatHasNotablyChanged(
        const sp<MetaData> &from, const sp<MetaData> &to) {
    if (from.get() == NULL && to.get() == NULL) {
        return false;
    }

    if ((from.get() == NULL && to.get() != NULL)
        || (from.get() != NULL && to.get() == NULL)) {
        return true;
    }

    const char *mime_from, *mime_to;
    CHECK(from->findCString(kKeyMIMEType, &mime_from));
    CHECK(to->findCString(kKeyMIMEType, &mime_to));

    if (strcasecmp(mime_from, mime_to)) {
        return true;
    }

    if (!strcasecmp(mime_from, MEDIA_MIMETYPE_VIDEO_RAW)) {
        int32_t colorFormat_from, colorFormat_to;
        CHECK(from->findInt32(kKeyColorFormat, &colorFormat_from));
        CHECK(to->findInt32(kKeyColorFormat, &colorFormat_to));

        if (colorFormat_from != colorFormat_to) {
            return true;
        }

        int32_t width_from, width_to;
        CHECK(from->findInt32(kKeyWidth, &width_from));
        CHECK(to->findInt32(kKeyWidth, &width_to));

        if (width_from != width_to) {
            return true;
        }

        int32_t height_from, height_to;
        CHECK(from->findInt32(kKeyHeight, &height_from));
        CHECK(to->findInt32(kKeyHeight, &height_to));

        if (height_from != height_to) {
            return true;
        }

        int32_t left_from, top_from, right_from, bottom_from;
        CHECK(from->findRect(
                    kKeyCropRect,
                    &left_from, &top_from, &right_from, &bottom_from));

        int32_t left_to, top_to, right_to, bottom_to;
        CHECK(to->findRect(
                    kKeyCropRect,
                    &left_to, &top_to, &right_to, &bottom_to));

        if (left_to != left_from || top_to != top_from
                || right_to != right_from || bottom_to != bottom_from) {
            return true;
        }
    } else if (!strcasecmp(mime_from, MEDIA_MIMETYPE_AUDIO_RAW)) {
        int32_t numChannels_from, numChannels_to;
        CHECK(from->findInt32(kKeyChannelCount, &numChannels_from));
        CHECK(to->findInt32(kKeyChannelCount, &numChannels_to));

        if (numChannels_from != numChannels_to) {
            return true;
        }

        int32_t sampleRate_from, sampleRate_to;
        CHECK(from->findInt32(kKeySampleRate, &sampleRate_from));
        CHECK(to->findInt32(kKeySampleRate, &sampleRate_to));

        if (sampleRate_from != sampleRate_to) {
            return true;
        }
    }

    return false;
}

void OMXCodec::onEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            onCmdComplete((OMX_COMMANDTYPE)data1, data2);
            break;
        }

        case OMX_EventError:
        {
            CODEC_LOGE("ERROR(0x%08lx, %ld)", data1, data2);

            setState(ERROR);
            break;
        }

        case OMX_EventPortSettingsChanged:
        {
            CODEC_LOGV("OMX_EventPortSettingsChanged(port=%ld, data2=0x%08lx)",
                       data1, data2);

            if (data2 == 0 || data2 == OMX_IndexParamPortDefinition) {
                // There is no need to check whether mFilledBuffers is empty or not
                // when the OMX_EventPortSettingsChanged is not meant for reallocating
                // the output buffers.
                if (data1 == kPortIndexOutput) {
                    CHECK(mFilledBuffers.empty());
                }
                onPortSettingsChanged(data1);
            } else if (data1 == kPortIndexOutput &&
                        (data2 == OMX_IndexConfigCommonOutputCrop ||
                         data2 == OMX_IndexConfigCommonScale)) {

                sp<MetaData> oldOutputFormat = mOutputFormat;
                initOutputFormat(mSource->getFormat());

                if (data2 == OMX_IndexConfigCommonOutputCrop &&
                    formatHasNotablyChanged(oldOutputFormat, mOutputFormat)) {
                    mOutputPortSettingsHaveChanged = true;

                } else if (data2 == OMX_IndexConfigCommonScale) {
                    OMX_CONFIG_SCALEFACTORTYPE scale;
                    InitOMXParams(&scale);
                    scale.nPortIndex = kPortIndexOutput;

                    // Change display dimension only when necessary.
                    if (OK == mOMX->getConfig(
                                        mNode,
                                        OMX_IndexConfigCommonScale,
                                        &scale, sizeof(scale))) {
                        int32_t left, top, right, bottom;
                        CHECK(mOutputFormat->findRect(kKeyCropRect,
                                                      &left, &top,
                                                      &right, &bottom));

                        // The scale is in 16.16 format.
                        // scale 1.0 = 0x010000. When there is no
                        // need to change the display, skip it.
                        ALOGV("Get OMX_IndexConfigScale: 0x%lx/0x%lx",
                                scale.xWidth, scale.xHeight);

                        if (scale.xWidth != 0x010000) {
                            mOutputFormat->setInt32(kKeyDisplayWidth,
                                    ((right - left +  1) * scale.xWidth)  >> 16);
                            mOutputPortSettingsHaveChanged = true;
                        }

                        if (scale.xHeight != 0x010000) {
                            mOutputFormat->setInt32(kKeyDisplayHeight,
                                    ((bottom  - top + 1) * scale.xHeight) >> 16);
                            mOutputPortSettingsHaveChanged = true;
                        }
                    }
                }
            }
            break;
        }

#if 0
        case OMX_EventBufferFlag:
        {
            CODEC_LOGV("EVENT_BUFFER_FLAG(%ld)", data1);

            if (data1 == kPortIndexOutput) {
                mNoMoreOutputData = true;
            }
            break;
        }
#endif

        default:
        {
            CODEC_LOGV("EVENT(%d, %ld, %ld)", event, data1, data2);
            break;
        }
    }
}

void OMXCodec::onCmdComplete(OMX_COMMANDTYPE cmd, OMX_U32 data) {
    switch (cmd) {
        case OMX_CommandStateSet:
        {
            onStateChange((OMX_STATETYPE)data);
            break;
        }

        case OMX_CommandPortDisable:
        {
            OMX_U32 portIndex = data;
            CODEC_LOGV("PORT_DISABLED(%ld)", portIndex);

            CHECK(mState == EXECUTING || mState == RECONFIGURING);
            CHECK_EQ((int)mPortStatus[portIndex], (int)DISABLING);
            CHECK_EQ(mPortBuffers[portIndex].size(), 0u);

            mPortStatus[portIndex] = DISABLED;

            if (mState == RECONFIGURING) {
                CHECK_EQ(portIndex, (OMX_U32)kPortIndexOutput);

                sp<MetaData> oldOutputFormat = mOutputFormat;
                initOutputFormat(mSource->getFormat());

                // Don't notify clients if the output port settings change
                // wasn't of importance to them, i.e. it may be that just the
                // number of buffers has changed and nothing else.
                bool formatChanged = formatHasNotablyChanged(oldOutputFormat, mOutputFormat);
                if (!mOutputPortSettingsHaveChanged) {
                    mOutputPortSettingsHaveChanged = formatChanged;
                }

                status_t err = enablePortAsync(portIndex);
                if (err != OK) {
                    CODEC_LOGE("enablePortAsync(%ld) failed (err = %d)", portIndex, err);
                    setState(ERROR);
                } else {
                    err = allocateBuffersOnPort(portIndex);
                    if (err != OK) {
                        CODEC_LOGE("allocateBuffersOnPort (%s) failed "
                                   "(err = %d)",
                                   portIndex == kPortIndexInput
                                        ? "input" : "output",
                                   err);

                        setState(ERROR);
                    }
                }
            }
            break;
        }

        case OMX_CommandPortEnable:
        {
            OMX_U32 portIndex = data;
            CODEC_LOGV("PORT_ENABLED(%ld)", portIndex);

            CHECK(mState == EXECUTING || mState == RECONFIGURING);
            CHECK_EQ((int)mPortStatus[portIndex], (int)ENABLING);

            mPortStatus[portIndex] = ENABLED;

            if (mState == RECONFIGURING) {
                CHECK_EQ(portIndex, (OMX_U32)kPortIndexOutput);

                setState(EXECUTING);

                fillOutputBuffers();
            }
            break;
        }

        case OMX_CommandFlush:
        {
            OMX_U32 portIndex = data;

            CODEC_LOGV("FLUSH_DONE(%ld)", portIndex);

            if (portIndex == (OMX_U32) -1) {
                CHECK_EQ((int)mPortStatus[kPortIndexInput], (int)SHUTTING_DOWN);
                mPortStatus[kPortIndexInput] = ENABLED;
                CHECK_EQ((int)mPortStatus[kPortIndexOutput], (int)SHUTTING_DOWN);
                mPortStatus[kPortIndexOutput] = ENABLED;
            } else {
                CHECK_EQ((int)mPortStatus[portIndex], (int)SHUTTING_DOWN);
                mPortStatus[portIndex] = ENABLED;

                CHECK_EQ(countBuffersWeOwn(mPortBuffers[portIndex]),
                         mPortBuffers[portIndex].size());
            }

            if (mSkipCutBuffer != NULL && mPortStatus[kPortIndexOutput] == ENABLED) {
                mSkipCutBuffer->clear();
            }

            if (mState == RECONFIGURING) {
                CHECK_EQ(portIndex, (OMX_U32)kPortIndexOutput);

                disablePortAsync(portIndex);
            } else if (mState == EXECUTING_TO_IDLE) {
                if (mPortStatus[kPortIndexInput] == ENABLED
                    && mPortStatus[kPortIndexOutput] == ENABLED) {
                    CODEC_LOGV("Finished flushing both ports, now completing "
                         "transition from EXECUTING to IDLE.");

                    mPortStatus[kPortIndexInput] = SHUTTING_DOWN;
                    mPortStatus[kPortIndexOutput] = SHUTTING_DOWN;

                    status_t err =
                        mOMX->sendCommand(mNode, OMX_CommandStateSet, OMX_StateIdle);
                    CHECK_EQ(err, (status_t)OK);
                }
            } else {
                // We're flushing both ports in preparation for seeking.

                if (mPortStatus[kPortIndexInput] == ENABLED
                    && mPortStatus[kPortIndexOutput] == ENABLED) {
                    setState(EXECUTING);
                    CODEC_LOGV("Finished flushing both ports, now continuing from"
                         " seek-time.");

                    // We implicitly resume pulling on our upstream source.
                    mPaused = false;

                    drainInputBuffers();
                    fillOutputBuffers();
                }

                if (mOutputPortSettingsChangedPending) {
                    CODEC_LOGV(
                            "Honoring deferred output port settings change.");

                    mOutputPortSettingsChangedPending = false;
                    onPortSettingsChanged(kPortIndexOutput);
                }
            }

            break;
        }

        default:
        {
            CODEC_LOGV("CMD_COMPLETE(%d, %ld)", cmd, data);
            break;
        }
    }
}

void OMXCodec::onStateChange(OMX_STATETYPE newState) {
    CODEC_LOGV("onStateChange %d", newState);

    switch (newState) {
        case OMX_StateIdle:
        {
            CODEC_LOGV("Now Idle.");
            if (mState == LOADED_TO_IDLE) {
                status_t err = mOMX->sendCommand(
                        mNode, OMX_CommandStateSet, OMX_StateExecuting);

                CHECK_EQ(err, (status_t)OK);

                //Both ports should be enabled by now
                mPortStatus[kPortIndexInput] = ENABLED;
                mPortStatus[kPortIndexOutput] = ENABLED;

                setState(IDLE_TO_EXECUTING);
            } else {
                CHECK_EQ((int)mState, (int)EXECUTING_TO_IDLE);

                if (countBuffersWeOwn(mPortBuffers[kPortIndexInput]) !=
                    mPortBuffers[kPortIndexInput].size()) {
                    ALOGE("Codec did not return all input buffers "
                          "(received %d / %d)",
                            countBuffersWeOwn(mPortBuffers[kPortIndexInput]),
                            mPortBuffers[kPortIndexInput].size());
                    TRESPASS();
                }

                if (countBuffersWeOwn(mPortBuffers[kPortIndexOutput]) !=
                    mPortBuffers[kPortIndexOutput].size()) {
                    ALOGE("Codec did not return all output buffers "
                          "(received %d / %d)",
                            countBuffersWeOwn(mPortBuffers[kPortIndexOutput]),
                            mPortBuffers[kPortIndexOutput].size());
                    TRESPASS();
                }

                status_t err = mOMX->sendCommand(
                        mNode, OMX_CommandStateSet, OMX_StateLoaded);

                CHECK_EQ(err, (status_t)OK);

                err = freeBuffersOnPort(kPortIndexInput);
                CHECK_EQ(err, (status_t)OK);

                err = freeBuffersOnPort(kPortIndexOutput);
                CHECK_EQ(err, (status_t)OK);

                mPortStatus[kPortIndexInput] = ENABLED;
                mPortStatus[kPortIndexOutput] = ENABLED;

                if ((mFlags & kEnableGrallocUsageProtected) &&
                        mNativeWindow != NULL) {
                    // We push enough 1x1 blank buffers to ensure that one of
                    // them has made it to the display.  This allows the OMX
                    // component teardown to zero out any protected buffers
                    // without the risk of scanning out one of those buffers.
                    pushBlankBuffersToNativeWindow();
                }

                setState(IDLE_TO_LOADED);
            }
            break;
        }

        case OMX_StateExecuting:
        {
            CHECK_EQ((int)mState, (int)IDLE_TO_EXECUTING);

            CODEC_LOGV("Now Executing.");

            mOutputPortSettingsChangedPending = false;

            setState(EXECUTING);

            // Buffers will be submitted to the component in the first
            // call to OMXCodec::read as mInitialBufferSubmit is true at
            // this point. This ensures that this on_message call returns,
            // releases the lock and ::init can notice the state change and
            // itself return.
            break;
        }

        case OMX_StateLoaded:
        {
            CHECK_EQ((int)mState, (int)IDLE_TO_LOADED);

            CODEC_LOGV("Now Loaded.");

            setState(LOADED);
            break;
        }

        case OMX_StatePause:
        {
            CODEC_LOGV("Now paused.");
            CHECK_EQ((int)mState, (int)PAUSING);
            setState(PAUSED);
            break;
        }

        case OMX_StateInvalid:
        {
            setState(ERROR);
            break;
        }

        default:
        {
            CHECK(!"should not be here.");
            break;
        }
    }
}

// static
size_t OMXCodec::countBuffersWeOwn(const Vector<BufferInfo> &buffers) {
    size_t n = 0;
    for (size_t i = 0; i < buffers.size(); ++i) {
        if (buffers[i].mStatus != OWNED_BY_COMPONENT) {
            ++n;
        }
    }

    return n;
}

status_t OMXCodec::freeBuffersOnPort(
        OMX_U32 portIndex, bool onlyThoseWeOwn) {
    Vector<BufferInfo> *buffers = &mPortBuffers[portIndex];

    status_t stickyErr = OK;

    for (size_t i = buffers->size(); i-- > 0;) {
        BufferInfo *info = &buffers->editItemAt(i);

        if (onlyThoseWeOwn && info->mStatus == OWNED_BY_COMPONENT) {
            continue;
        }

        CHECK(info->mStatus == OWNED_BY_US
                || info->mStatus == OWNED_BY_NATIVE_WINDOW);

        CODEC_LOGV("freeing buffer %p on port %ld", info->mBuffer, portIndex);

        status_t err = freeBuffer(portIndex, i);

        if (err != OK) {
            stickyErr = err;
        }

    }

    CHECK(onlyThoseWeOwn || buffers->isEmpty());

    return stickyErr;
}

status_t OMXCodec::freeBuffer(OMX_U32 portIndex, size_t bufIndex) {
    Vector<BufferInfo> *buffers = &mPortBuffers[portIndex];

    BufferInfo *info = &buffers->editItemAt(bufIndex);

    status_t err = mOMX->freeBuffer(mNode, portIndex, info->mBuffer);

    if (err == OK && info->mMediaBuffer != NULL) {
        CHECK_EQ(portIndex, (OMX_U32)kPortIndexOutput);
        info->mMediaBuffer->setObserver(NULL);

        // Make sure nobody but us owns this buffer at this point.
        CHECK_EQ(info->mMediaBuffer->refcount(), 0);

        // Cancel the buffer if it belongs to an ANativeWindow.
        sp<GraphicBuffer> graphicBuffer = info->mMediaBuffer->graphicBuffer();
        if (info->mStatus == OWNED_BY_US && graphicBuffer != 0) {
            err = cancelBufferToNativeWindow(info);
        }

        info->mMediaBuffer->release();
        info->mMediaBuffer = NULL;
    }

    if (err == OK) {
        buffers->removeAt(bufIndex);
    }

    return err;
}

void OMXCodec::onPortSettingsChanged(OMX_U32 portIndex) {
    CODEC_LOGV("PORT_SETTINGS_CHANGED(%ld)", portIndex);

    CHECK_EQ((int)mState, (int)EXECUTING);
    CHECK_EQ(portIndex, (OMX_U32)kPortIndexOutput);
    CHECK(!mOutputPortSettingsChangedPending);

    if (mPortStatus[kPortIndexOutput] != ENABLED) {
        CODEC_LOGV("Deferring output port settings change.");
        mOutputPortSettingsChangedPending = true;
        return;
    }

    setState(RECONFIGURING);

    if (mQuirks & kNeedsFlushBeforeDisable) {
        if (!flushPortAsync(portIndex)) {
            onCmdComplete(OMX_CommandFlush, portIndex);
        }
    } else {
        disablePortAsync(portIndex);
    }
}

bool OMXCodec::flushPortAsync(OMX_U32 portIndex) {
    CHECK(mState == EXECUTING || mState == RECONFIGURING
            || mState == FLUSHING
            || mState == EXECUTING_TO_IDLE);

    if (portIndex == (OMX_U32) -1 ) {
        mPortStatus[kPortIndexInput] = SHUTTING_DOWN;
        mPortStatus[kPortIndexOutput] = SHUTTING_DOWN;
    } else {
        CODEC_LOGV("flushPortAsync(%ld): we own %d out of %d buffers already.",
             portIndex, countBuffersWeOwn(mPortBuffers[portIndex]),
             mPortBuffers[portIndex].size());

        CHECK_EQ((int)mPortStatus[portIndex], (int)ENABLED);
        mPortStatus[portIndex] = SHUTTING_DOWN;

        if ((mQuirks & kRequiresFlushCompleteEmulation)
            && countBuffersWeOwn(mPortBuffers[portIndex])
                    == mPortBuffers[portIndex].size()) {
            // No flush is necessary and this component fails to send a
            // flush-complete event in this case.

            return false;
        }
    }

    status_t err =
        mOMX->sendCommand(mNode, OMX_CommandFlush, portIndex);
    CHECK_EQ(err, (status_t)OK);

    return true;
}

void OMXCodec::disablePortAsync(OMX_U32 portIndex) {
    CHECK(mState == EXECUTING || mState == RECONFIGURING);

    CHECK_EQ((int)mPortStatus[portIndex], (int)ENABLED);
    mPortStatus[portIndex] = DISABLING;

    CODEC_LOGV("sending OMX_CommandPortDisable(%ld)", portIndex);
    status_t err =
        mOMX->sendCommand(mNode, OMX_CommandPortDisable, portIndex);
    CHECK_EQ(err, (status_t)OK);

    freeBuffersOnPort(portIndex, true);
}

status_t OMXCodec::enablePortAsync(OMX_U32 portIndex) {
    CHECK(mState == EXECUTING || mState == RECONFIGURING);

    CHECK_EQ((int)mPortStatus[portIndex], (int)DISABLED);
    mPortStatus[portIndex] = ENABLING;

    CODEC_LOGV("sending OMX_CommandPortEnable(%ld)", portIndex);
    return mOMX->sendCommand(mNode, OMX_CommandPortEnable, portIndex);
}

void OMXCodec::fillOutputBuffers() {
    CHECK(mState == EXECUTING || mState == FLUSHING);

    // This is a workaround for some decoders not properly reporting
    // end-of-output-stream. If we own all input buffers and also own
    // all output buffers and we already signalled end-of-input-stream,
    // the end-of-output-stream is implied.

    if (mSignalledEOS
            && countBuffersWeOwn(mPortBuffers[kPortIndexInput])
                == mPortBuffers[kPortIndexInput].size()
            && countBuffersWeOwn(mPortBuffers[kPortIndexOutput])
                == mPortBuffers[kPortIndexOutput].size()) {
        mNoMoreOutputData = true;
        mBufferFilled.signal();

            return;
    }

    Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexOutput];
    for (size_t i = 0; i < buffers->size(); ++i) {
        BufferInfo *info = &buffers->editItemAt(i);
        if (info->mStatus == OWNED_BY_US) {
            fillOutputBuffer(&buffers->editItemAt(i));
        }
    }
}

void OMXCodec::drainInputBuffers() {
    CHECK(mState == EXECUTING || mState == RECONFIGURING || mState == FLUSHING);

    if (mFlags & kUseSecureInputBuffers) {
        Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexInput];
        for (size_t i = 0; i < buffers->size(); ++i) {
            if (!drainAnyInputBuffer()
                    || (mFlags & kOnlySubmitOneInputBufferAtOneTime)) {
                break;
            }
        }
    } else {
        Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexInput];
        for (size_t i = 0; i < buffers->size(); ++i) {
            BufferInfo *info = &buffers->editItemAt(i);

            if (info->mStatus != OWNED_BY_US) {
                continue;
            }

#if defined OMAP_ENHANCEMENT && defined TARGET_OMAP3
            if (mIsEncoder && mIsVideo && (i == 4)) {
                break;
            }
#endif
            if (!drainInputBuffer(info)) {
                break;
            }

            if (mFlags & kOnlySubmitOneInputBufferAtOneTime) {
                break;
            }
        }
    }
}

bool OMXCodec::drainAnyInputBuffer() {
    return drainInputBuffer((BufferInfo *)NULL);
}

OMXCodec::BufferInfo *OMXCodec::findInputBufferByDataPointer(void *ptr) {
    Vector<BufferInfo> *infos = &mPortBuffers[kPortIndexInput];
    for (size_t i = 0; i < infos->size(); ++i) {
        BufferInfo *info = &infos->editItemAt(i);

        if (info->mData == ptr) {
            CODEC_LOGV(
                    "input buffer data ptr = %p, buffer_id = %p",
                    ptr,
                    info->mBuffer);

            return info;
        }
    }

    TRESPASS();
}

OMXCodec::BufferInfo *OMXCodec::findEmptyInputBuffer() {
    Vector<BufferInfo> *infos = &mPortBuffers[kPortIndexInput];
    for (size_t i = 0; i < infos->size(); ++i) {
        BufferInfo *info = &infos->editItemAt(i);

        if (info->mStatus == OWNED_BY_US) {
            return info;
        }
    }

    TRESPASS();
}

bool OMXCodec::drainInputBuffer(BufferInfo *info) {
    ATRACE_CALL();
    if (info != NULL) {
        CHECK_EQ((int)info->mStatus, (int)OWNED_BY_US);
    }

    if (mSignalledEOS) {
        return false;
    }

    if (mCodecSpecificDataIndex < mCodecSpecificData.size()) {
        CHECK(!(mFlags & kUseSecureInputBuffers));

        const CodecSpecificData *specific =
            mCodecSpecificData[mCodecSpecificDataIndex];

        size_t size = specific->mSize;

        if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mMIME)
                && !(mQuirks & kWantsNALFragments)) {
            static const uint8_t kNALStartCode[4] =
                    { 0x00, 0x00, 0x00, 0x01 };

            CHECK(info->mSize >= specific->mSize + 4);

            size += 4;

            memcpy(info->mData, kNALStartCode, 4);
            memcpy((uint8_t *)info->mData + 4,
                   specific->mData, specific->mSize);
        } else {
            CHECK(info->mSize >= specific->mSize);
            memcpy(info->mData, specific->mData, specific->mSize);
        }

        mNoMoreOutputData = false;

        CODEC_LOGV("calling emptyBuffer with codec specific data");

        status_t err = mOMX->emptyBuffer(
                mNode, info->mBuffer, 0, size,
                OMX_BUFFERFLAG_ENDOFFRAME | OMX_BUFFERFLAG_CODECCONFIG,
                0);
        CHECK_EQ(err, (status_t)OK);

        info->mStatus = OWNED_BY_COMPONENT;

        ++mCodecSpecificDataIndex;
        return true;
    }

    if (mPaused) {
        return false;
    }

    status_t err;

    bool signalEOS = false;
    int64_t timestampUs = 0;

    size_t offset = 0;
    int32_t n = 0;

    for (;;) {
        MediaBuffer *srcBuffer;
        if (mSeekTimeUs >= 0) {
            if (mLeftOverBuffer) {
                mLeftOverBuffer->release();
                mLeftOverBuffer = NULL;
            }

            MediaSource::ReadOptions options;
            options.setSeekTo(mSeekTimeUs, mSeekMode);

            mSeekTimeUs = -1;
            mSeekMode = ReadOptions::SEEK_CLOSEST_SYNC;
            mBufferFilled.signal();

            err = mSource->read(&srcBuffer, &options);

            if (err == OK) {
                int64_t targetTimeUs;
                if (srcBuffer->meta_data()->findInt64(
                            kKeyTargetTime, &targetTimeUs)
                        && targetTimeUs >= 0) {
                    CODEC_LOGV("targetTimeUs = %lld us", targetTimeUs);
                    mTargetTimeUs = targetTimeUs;
                } else {
                    mTargetTimeUs = -1;
                }
            }
        } else if (mLeftOverBuffer) {
            srcBuffer = mLeftOverBuffer;
            mLeftOverBuffer = NULL;

            err = OK;
        } else {
            err = mSource->read(&srcBuffer);
        }

        if (err == -EAGAIN) {
            mSignalledReadTryAgain = true;
            mBufferFilled.signal();
            return false;
        } else {
            mSignalledReadTryAgain = false;
        }

        if (err != OK) {
            signalEOS = true;
            mFinalStatus = err;
            mSignalledEOS = true;
            mBufferFilled.signal();
            break;
        }

        if (mFlags & kUseSecureInputBuffers) {
            info = findInputBufferByDataPointer(srcBuffer->data());
            CHECK(info != NULL);
        }

        size_t remainingBytes = info->mSize - offset;

        if (srcBuffer->range_length() > remainingBytes) {
            if (offset == 0) {
                CODEC_LOGE(
                     "Codec's input buffers are too small to accomodate "
                     "buffer read from source (info->mSize = %d, srcLength = %d)",
                     info->mSize, srcBuffer->range_length());

                srcBuffer->release();
                srcBuffer = NULL;

                setState(ERROR);
                return false;
            }

            mLeftOverBuffer = srcBuffer;
            break;
        }

        bool releaseBuffer = true;
        if (mFlags & kStoreMetaDataInVideoBuffers) {
                releaseBuffer = false;
                info->mMediaBuffer = srcBuffer;
        }

        if (mFlags & kUseSecureInputBuffers) {
                // Data in "info" is already provided at this time.

                releaseBuffer = false;

                CHECK(info->mMediaBuffer == NULL);
                info->mMediaBuffer = srcBuffer;
#ifdef OMAP_ENHANCEMENT
        } else if (mIsEncoder && (mQuirks & kAvoidMemcopyInputRecordingFrames)) {
                CHECK(mOMXLivesLocally && offset == 0);

                OMX_BUFFERHEADERTYPE *header =
                    (OMX_BUFFERHEADERTYPE *)info->mBuffer;

                CHECK(header->pBuffer == info->mData);

                header->pBuffer =
                    (OMX_U8 *)srcBuffer->data() + srcBuffer->range_offset();

                releaseBuffer = false;
                info->mMediaBuffer = srcBuffer;
#endif
        } else {
#ifdef USE_SAMSUNG_COLORFORMAT
            OMX_PARAM_PORTDEFINITIONTYPE def;
            InitOMXParams(&def);
            def.nPortIndex = kPortIndexInput;

            status_t err = mOMX->getParameter(mNode, OMX_IndexParamPortDefinition,
            &def, sizeof(def));
            CHECK_EQ(err, (status_t)OK);

            if (def.eDomain == OMX_PortDomainVideo) {
                OMX_VIDEO_PORTDEFINITIONTYPE *videoDef = &def.format.video;
                switch (videoDef->eColorFormat) {
                    case OMX_SEC_COLOR_FormatNV12LVirtualAddress: {
                        CHECK(srcBuffer->data() != NULL);
                        void *pSharedMem = (void *)(srcBuffer->data());
                        memcpy((uint8_t *)info->mData + offset,
                        (const void *)&pSharedMem, sizeof(void *));
                        break;
                    }
                    default:
                        CHECK(srcBuffer->data() != NULL);
                        memcpy((uint8_t *)info->mData + offset,
                        (const uint8_t *)srcBuffer->data()
                        + srcBuffer->range_offset(),
                        srcBuffer->range_length());
                        break;
                    }
            } else {
                CHECK(srcBuffer->data() != NULL);
                memcpy((uint8_t *)info->mData + offset,
                        (const uint8_t *)srcBuffer->data()
                            + srcBuffer->range_offset(),
                        srcBuffer->range_length());
            }
#else
            CHECK(srcBuffer->data() != NULL) ;
            memcpy((uint8_t *)info->mData + offset,
                    (const uint8_t *)srcBuffer->data()
                        + srcBuffer->range_offset(),
                    srcBuffer->range_length());
#endif // USE_SAMSUNG_COLORFORMAT
        }

        int64_t lastBufferTimeUs;
        CHECK(srcBuffer->meta_data()->findInt64(kKeyTime, &lastBufferTimeUs));
        CHECK(lastBufferTimeUs >= 0);
        if (mIsEncoder && mIsVideo) {
            mDecodingTimeList.push_back(lastBufferTimeUs);
        }

        if (offset == 0) {
            timestampUs = lastBufferTimeUs;
        }

        offset += srcBuffer->range_length();

        if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_VORBIS, mMIME)) {
            CHECK(!(mQuirks & kSupportsMultipleFramesPerInputBuffer));
            CHECK_GE(info->mSize, offset + sizeof(int32_t));

            int32_t numPageSamples;
            if (!srcBuffer->meta_data()->findInt32(
                        kKeyValidSamples, &numPageSamples)) {
                numPageSamples = -1;
            }

            memcpy((uint8_t *)info->mData + offset,
                   &numPageSamples,
                   sizeof(numPageSamples));

            offset += sizeof(numPageSamples);
        }

        if (releaseBuffer) {
            srcBuffer->release();
            srcBuffer = NULL;
        }

        ++n;

        if (!(mQuirks & kSupportsMultipleFramesPerInputBuffer)) {
            break;
        }

        int64_t coalescedDurationUs = lastBufferTimeUs - timestampUs;

        if (coalescedDurationUs > 250000ll) {
            // Don't coalesce more than 250ms worth of encoded data at once.
            break;
        }
    }

    if (n > 1) {
        ALOGV("coalesced %d frames into one input buffer", n);
    }

    OMX_U32 flags = OMX_BUFFERFLAG_ENDOFFRAME;

    if (signalEOS) {
        flags |= OMX_BUFFERFLAG_EOS;
    } else {
        mNoMoreOutputData = false;
    }

    if (info == NULL) {
        CHECK(mFlags & kUseSecureInputBuffers);
        CHECK(signalEOS);

        // This is fishy, there's still a MediaBuffer corresponding to this
        // info available to the source at this point even though we're going
        // to use it to signal EOS to the codec.
        info = findEmptyInputBuffer();
    }

    CODEC_LOGV("Calling emptyBuffer on buffer %p (length %d), "
               "timestamp %lld us (%.2f secs)",
               info->mBuffer, offset,
               timestampUs, timestampUs / 1E6);

    err = mOMX->emptyBuffer(
            mNode, info->mBuffer, 0, offset,
            flags, timestampUs);

    if (err != OK) {
        setState(ERROR);
        return false;
    }

    // This component does not ever signal the EOS flag on output buffers,
    // Thanks for nothing.
    if (mSignalledEOS && (!strcmp(mComponentName, "OMX.TI.Video.encoder") || 
                          !strcmp(mComponentName, "OMX.TI.720P.Encoder"))) {
        mNoMoreOutputData = true;
        mBufferFilled.signal();
    }

    info->mStatus = OWNED_BY_COMPONENT;

    return true;
}

void OMXCodec::fillOutputBuffer(BufferInfo *info) {
    CHECK_EQ((int)info->mStatus, (int)OWNED_BY_US);

    if (mNoMoreOutputData) {
        CODEC_LOGV("There is no more output data available, not "
             "calling fillOutputBuffer");
        return;
    }

    CODEC_LOGV("Calling fillBuffer on buffer %p", info->mBuffer);
    status_t err = mOMX->fillBuffer(mNode, info->mBuffer);

    if (err != OK) {
        CODEC_LOGE("fillBuffer failed w/ error 0x%08x", err);

        setState(ERROR);
        return;
    }

    info->mStatus = OWNED_BY_COMPONENT;
}

bool OMXCodec::drainInputBuffer(IOMX::buffer_id buffer) {
    Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexInput];
    for (size_t i = 0; i < buffers->size(); ++i) {
        if ((*buffers)[i].mBuffer == buffer) {
            return drainInputBuffer(&buffers->editItemAt(i));
        }
    }

    CHECK(!"should not be here.");

    return false;
}

void OMXCodec::fillOutputBuffer(IOMX::buffer_id buffer) {
    Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexOutput];
    for (size_t i = 0; i < buffers->size(); ++i) {
        if ((*buffers)[i].mBuffer == buffer) {
            fillOutputBuffer(&buffers->editItemAt(i));
            return;
        }
    }

    CHECK(!"should not be here.");
}

void OMXCodec::setState(State newState) {
    mState = newState;
    mAsyncCompletion.signal();

    // This may cause some spurious wakeups but is necessary to
    // unblock the reader if we enter ERROR state.
    mBufferFilled.signal();
}

status_t OMXCodec::waitForBufferFilled_l() {

    ATRACE_CALL();
    if (mIsEncoder && mIsVideo) {
        // For timelapse video recording, the timelapse video recording may
        // not send an input frame for a _long_ time. Do not use timeout
        // for video encoding.
        return mBufferFilled.wait(mLock);
    }

    if ((mState == EXECUTING || mState == FLUSHING) && (mSignalledReadTryAgain == true)) {
        return -EAGAIN;
    }

    status_t err = mBufferFilled.waitRelative(mLock, kBufferFilledEventTimeOutNs);
    if ((err == -ETIMEDOUT) && (mPaused == true)){
        err = OK;
    }

    if ((err == OK) && (mSignalledReadTryAgain == true) && (mState == EXECUTING || mState == FLUSHING)) {
        return -EAGAIN;
    }

    if (err != OK) {
        CODEC_LOGE("Timed out waiting for output buffers: %d/%d",
            countBuffersWeOwn(mPortBuffers[kPortIndexInput]),
            countBuffersWeOwn(mPortBuffers[kPortIndexOutput]));
    }
    return err;
}

void OMXCodec::setRawAudioFormat(
        OMX_U32 portIndex, int32_t sampleRate, int32_t numChannels) {

    // port definition
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;
#ifdef QCOM_HARDWARE
    def.format.audio.cMIMEType = NULL;
#endif
    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);
    def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;
    CHECK_EQ(mOMX->setParameter(mNode, OMX_IndexParamPortDefinition,
            &def, sizeof(def)), (status_t)OK);

    // pcm param
    OMX_AUDIO_PARAM_PCMMODETYPE pcmParams;
    InitOMXParams(&pcmParams);
    pcmParams.nPortIndex = portIndex;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));

    CHECK_EQ(err, (status_t)OK);

    pcmParams.nChannels = numChannels;
    pcmParams.eNumData = OMX_NumericalDataSigned;
    pcmParams.bInterleaved = OMX_TRUE;
    pcmParams.nBitPerSample = 16;
    pcmParams.nSamplingRate = sampleRate;
    pcmParams.ePCMMode = OMX_AUDIO_PCMModeLinear;

    CHECK_EQ(getOMXChannelMapping(
                numChannels, pcmParams.eChannelMapping), (status_t)OK);

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));

    CHECK_EQ(err, (status_t)OK);
}

static OMX_AUDIO_AMRBANDMODETYPE pickModeFromBitRate(bool isAMRWB, int32_t bps) {
    if (isAMRWB) {
        if (bps <= 6600) {
            return OMX_AUDIO_AMRBandModeWB0;
        } else if (bps <= 8850) {
            return OMX_AUDIO_AMRBandModeWB1;
        } else if (bps <= 12650) {
            return OMX_AUDIO_AMRBandModeWB2;
        } else if (bps <= 14250) {
            return OMX_AUDIO_AMRBandModeWB3;
        } else if (bps <= 15850) {
            return OMX_AUDIO_AMRBandModeWB4;
        } else if (bps <= 18250) {
            return OMX_AUDIO_AMRBandModeWB5;
        } else if (bps <= 19850) {
            return OMX_AUDIO_AMRBandModeWB6;
        } else if (bps <= 23050) {
            return OMX_AUDIO_AMRBandModeWB7;
        }

        // 23850 bps
        return OMX_AUDIO_AMRBandModeWB8;
    } else {  // AMRNB
        if (bps <= 4750) {
            return OMX_AUDIO_AMRBandModeNB0;
        } else if (bps <= 5150) {
            return OMX_AUDIO_AMRBandModeNB1;
        } else if (bps <= 5900) {
            return OMX_AUDIO_AMRBandModeNB2;
        } else if (bps <= 6700) {
            return OMX_AUDIO_AMRBandModeNB3;
        } else if (bps <= 7400) {
            return OMX_AUDIO_AMRBandModeNB4;
        } else if (bps <= 7950) {
            return OMX_AUDIO_AMRBandModeNB5;
        } else if (bps <= 10200) {
            return OMX_AUDIO_AMRBandModeNB6;
        }

        // 12200 bps
        return OMX_AUDIO_AMRBandModeNB7;
    }
}

void OMXCodec::setAMRFormat(bool isWAMR, int32_t bitRate) {
    OMX_U32 portIndex = mIsEncoder ? kPortIndexOutput : kPortIndexInput;

    OMX_AUDIO_PARAM_AMRTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    status_t err =
        mOMX->getParameter(mNode, OMX_IndexParamAudioAmr, &def, sizeof(def));

    CHECK_EQ(err, (status_t)OK);

    def.eAMRFrameFormat = OMX_AUDIO_AMRFrameFormatFSF;

    def.eAMRBandMode = pickModeFromBitRate(isWAMR, bitRate);
    err = mOMX->setParameter(mNode, OMX_IndexParamAudioAmr, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    ////////////////////////

    if (mIsEncoder) {
        sp<MetaData> format = mSource->getFormat();
        int32_t sampleRate;
        int32_t numChannels;
        CHECK(format->findInt32(kKeySampleRate, &sampleRate));
        CHECK(format->findInt32(kKeyChannelCount, &numChannels));

        setRawAudioFormat(kPortIndexInput, sampleRate, numChannels);
    }
}

status_t OMXCodec::setAACFormat(
        int32_t numChannels, int32_t sampleRate, int32_t bitRate, int32_t aacProfile, bool isADTS) {
    if (numChannels > 2) {
        ALOGW("Number of channels: (%d) \n", numChannels);
    }

    if (mIsEncoder) {
        if (isADTS) {
            return -EINVAL;
        }

        //////////////// input port ////////////////////
        setRawAudioFormat(kPortIndexInput, sampleRate, numChannels);

        //////////////// output port ////////////////////
        // format
        OMX_AUDIO_PARAM_PORTFORMATTYPE format;
        InitOMXParams(&format);
        format.nPortIndex = kPortIndexOutput;
        format.nIndex = 0;
        status_t err = OMX_ErrorNone;
        while (OMX_ErrorNone == err) {
            CHECK_EQ(mOMX->getParameter(mNode, OMX_IndexParamAudioPortFormat,
                    &format, sizeof(format)), (status_t)OK);
            if (format.eEncoding == OMX_AUDIO_CodingAAC) {
                break;
            }
            format.nIndex++;
        }
        CHECK_EQ((status_t)OK, err);
        CHECK_EQ(mOMX->setParameter(mNode, OMX_IndexParamAudioPortFormat,
                &format, sizeof(format)), (status_t)OK);

        // port definition
        OMX_PARAM_PORTDEFINITIONTYPE def;
        InitOMXParams(&def);
        def.nPortIndex = kPortIndexOutput;
        CHECK_EQ(mOMX->getParameter(mNode, OMX_IndexParamPortDefinition,
                &def, sizeof(def)), (status_t)OK);
        def.format.audio.bFlagErrorConcealment = OMX_TRUE;
        def.format.audio.eEncoding = OMX_AUDIO_CodingAAC;
        CHECK_EQ(mOMX->setParameter(mNode, OMX_IndexParamPortDefinition,
                &def, sizeof(def)), (status_t)OK);

        // profile
        OMX_AUDIO_PARAM_AACPROFILETYPE profile;
        InitOMXParams(&profile);
        profile.nPortIndex = kPortIndexOutput;
        CHECK_EQ(mOMX->getParameter(mNode, OMX_IndexParamAudioAac,
                &profile, sizeof(profile)), (status_t)OK);
        profile.nChannels = numChannels;
        profile.eChannelMode = (numChannels == 1?
                OMX_AUDIO_ChannelModeMono: OMX_AUDIO_ChannelModeStereo);
        profile.nSampleRate = sampleRate;
        profile.nBitRate = bitRate;
        profile.nAudioBandWidth = 0;
        profile.nFrameLength = 0;
        profile.nAACtools = OMX_AUDIO_AACToolAll;
        profile.nAACERtools = OMX_AUDIO_AACERNone;
        profile.eAACProfile = (OMX_AUDIO_AACPROFILETYPE) aacProfile;
        profile.eAACStreamFormat = OMX_AUDIO_AACStreamFormatMP4FF;
        err = mOMX->setParameter(mNode, OMX_IndexParamAudioAac,
                &profile, sizeof(profile));

        if (err != OK) {
            CODEC_LOGE("setParameter('OMX_IndexParamAudioAac') failed "
                       "(err = %d)",
                       err);
            return err;
        }
    } else {
        OMX_AUDIO_PARAM_AACPROFILETYPE profile;
        InitOMXParams(&profile);
        profile.nPortIndex = kPortIndexInput;

        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamAudioAac, &profile, sizeof(profile));
        CHECK_EQ(err, (status_t)OK);

        profile.nChannels = numChannels;
        profile.nSampleRate = sampleRate;

        profile.eAACStreamFormat =
            isADTS
                ? OMX_AUDIO_AACStreamFormatMP4ADTS
                : OMX_AUDIO_AACStreamFormatMP4FF;

        err = mOMX->setParameter(
                mNode, OMX_IndexParamAudioAac, &profile, sizeof(profile));

        if (err != OK) {
            CODEC_LOGE("setParameter('OMX_IndexParamAudioAac') failed "
                       "(err = %d)",
                       err);
            return err;
        }
    }

    return OK;
}

//video
status_t OMXCodec::setWMVFormat(const sp<MetaData> &meta)
{
    int32_t version = 0;
    OMX_VIDEO_PARAM_WMVTYPE paramWMV;

    if (mIsEncoder) {
        CODEC_LOGE("WMV encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyWMVVersion, &version));

    InitOMXParams(&paramWMV);
    paramWMV.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoWmv, &paramWMV, sizeof(paramWMV));
    if (err != OK)
        return err;

    if (version == kTypeWMVVer_7) {
        paramWMV.eFormat = OMX_VIDEO_WMVFormat7;
    } else if (version == kTypeWMVVer_8) {
        paramWMV.eFormat = OMX_VIDEO_WMVFormat8;
    } else if (version == kTypeWMVVer_9) {
        paramWMV.eFormat = OMX_VIDEO_WMVFormat9;
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoWmv, &paramWMV, sizeof(paramWMV));
    return err;
}

status_t OMXCodec::setRVFormat(const sp<MetaData> &meta)
{
    int32_t version = 0;
    OMX_VIDEO_PARAM_RVTYPE paramRV;

    if (mIsEncoder) {
        CODEC_LOGE("RV encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyRVVersion, &version));

    InitOMXParams(&paramRV);
    paramRV.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoRv, &paramRV, sizeof(paramRV));
    if (err != OK)
        return err;

    if (version == kTypeRVVer_G2) {
        paramRV.eFormat = OMX_VIDEO_RVFormatG2;
    } else if (version == kTypeRVVer_8) {
        paramRV.eFormat = OMX_VIDEO_RVFormat8;
    } else if (version == kTypeRVVer_9) {
        paramRV.eFormat = OMX_VIDEO_RVFormat9;
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoRv, &paramRV, sizeof(paramRV));
    return err;
}

status_t OMXCodec::setFFmpegVideoFormat(const sp<MetaData> &meta)
{
    int32_t codec_id = 0;
    int32_t width = 0;
    int32_t height = 0;
    OMX_VIDEO_PARAM_FFMPEGTYPE param;

    ALOGD("setFFmpegVideoFormat");

    if (mIsEncoder) {
        CODEC_LOGE("FFMPEG encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyCodecId, &codec_id));
    CHECK(meta->findInt32(kKeyWidth, &width));
    CHECK(meta->findInt32(kKeyHeight, &height));

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoFFmpeg, &param, sizeof(param));
    if (err != OK)
        return err;

    param.eCodecId = codec_id;
    param.nWidth   = width;
    param.nHeight  = height;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoFFmpeg, &param, sizeof(param));
    return err;
}

//audio
status_t OMXCodec::setMP3Format(const sp<MetaData> &meta)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    OMX_AUDIO_PARAM_MP3TYPE param;

    // skip if not OMX.ffmpeg.mp3.decoder
    if (strncmp(mComponentName, "OMX.ffmpeg.mp3.decoder", 22)) {
        int32_t numChannels, sampleRate;
        if (meta->findInt32(kKeyChannelCount, &numChannels)
                && meta->findInt32(kKeySampleRate, &sampleRate)) {
            // Since we did not always check for these, leave them optional
            // and have the decoder figure it all out.
            setRawAudioFormat(
                    mIsEncoder ? kPortIndexInput : kPortIndexOutput,
                    sampleRate,
                    numChannels);
        }
        return OK;
    }

    if (mIsEncoder) {
        CODEC_LOGE("MP3 encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));

    CODEC_LOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioMp3, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioMp3, &param, sizeof(param));
    return err;
}

status_t OMXCodec::setWMAFormat(const sp<MetaData> &meta)
{
    int32_t version = 0;
    int32_t numChannels = 0;
    int32_t bitRate = 0;
    int32_t sampleRate = 0;
    int32_t blockAlign = 0;
    int32_t formattag = 0;
    OMX_AUDIO_PARAM_WMATYPE paramWMA;

    if (mIsEncoder) {
        CODEC_LOGE("WMA encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
    CHECK(meta->findInt32(kKeyBitRate, &bitRate));
    CHECK(meta->findInt32(kKeyBlockAlign, &blockAlign));

    CODEC_LOGV("Channels: %d, SampleRate: %d, BitRate: %d, blockAlign: %d",
            numChannels, sampleRate, bitRate, blockAlign);

    CHECK(meta->findInt32(kKeyWMAVersion, &version));

    InitOMXParams(&paramWMA);
    paramWMA.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioWma, &paramWMA, sizeof(paramWMA));
    if (err != OK)
        return err;

    paramWMA.nChannels = numChannels;
    paramWMA.nSamplingRate = sampleRate;
    paramWMA.nBitRate = bitRate;
    paramWMA.nBlockAlign = blockAlign;

    // http://msdn.microsoft.com/en-us/library/ff819498(v=vs.85).aspx
    if (version == kTypeWMA) {
        paramWMA.eFormat = OMX_AUDIO_WMAFormat7;
    } else if (version == kTypeWMAPro) {
        paramWMA.eFormat = OMX_AUDIO_WMAFormat8;
    } else if (version == kTypeWMALossLess) {
        paramWMA.eFormat = OMX_AUDIO_WMAFormat9;
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioWma, &paramWMA, sizeof(paramWMA));
    return err;
}

status_t OMXCodec::setVORBISFormat(const sp<MetaData> &meta)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    OMX_AUDIO_PARAM_VORBISTYPE param;

    if (mIsEncoder) {
        CODEC_LOGE("VORBIS encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));

    CODEC_LOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioVorbis, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioVorbis, &param, sizeof(param));
    return err;
}

status_t OMXCodec::setRAFormat(const sp<MetaData> &meta)
{
    int32_t numChannels = 0;
    int32_t bitRate = 0;
    int32_t sampleRate = 0;
    int32_t blockAlign = 0;
    OMX_AUDIO_PARAM_RATYPE paramRA;

    if (mIsEncoder) {
        CODEC_LOGE("RA encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
    CHECK(meta->findInt32(kKeyBitRate, &bitRate));
    CHECK(meta->findInt32(kKeyBlockAlign, &blockAlign));

    CODEC_LOGV("Channels: %d, SampleRate: %d, BitRate: %d, blockAlign: %d",
            numChannels, sampleRate, bitRate, blockAlign);

    InitOMXParams(&paramRA);
    paramRA.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioRa, &paramRA, sizeof(paramRA));
    if (err != OK)
        return err;

    paramRA.eFormat = OMX_AUDIO_RAFormatUnused; // FIXME, cook only???
    paramRA.nChannels = numChannels;
    paramRA.nSamplingRate = sampleRate;
    // FIXME, HACK!!!, I use the nNumRegions parameter pass blockAlign!!!
    // the cook audio codec need blockAlign!
    paramRA.nNumRegions = blockAlign;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioRa, &paramRA, sizeof(paramRA));
    return err;
}

status_t OMXCodec::setFLACFormat(const sp<MetaData> &meta)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 16;
    OMX_AUDIO_PARAM_FLACTYPE param;

    if (mIsEncoder) {
        CODEC_LOGE("FLAC encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
    if (!meta->findInt32(kKeySampleBits, &bitsPerSample)) {
        CODEC_LOGV("BitsPerSample not set, using default");
    }

    CODEC_LOGV("Channels: %d, SampleRate: %d, BitsPerSample: %d",
            numChannels, sampleRate, bitsPerSample);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioFlac, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;
    param.nBitsPerSample = bitsPerSample;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioFlac, &param, sizeof(param));
    return err;
}

status_t OMXCodec::setMP2Format(const sp<MetaData> &meta)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    OMX_AUDIO_PARAM_MP2TYPE param;

    if (mIsEncoder) {
        CODEC_LOGE("MP2 encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));

    CODEC_LOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioMp2, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSampleRate = sampleRate;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioMp2, &param, sizeof(param));
    return err;
}

status_t OMXCodec::setAC3Format(const sp<MetaData> &meta)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    OMX_AUDIO_PARAM_AC3TYPE param;

    if (mIsEncoder) {
        CODEC_LOGE("AC3 encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));

    CODEC_LOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioAc3, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSamplingRate = sampleRate;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioAc3, &param, sizeof(param));
    return err;
}

status_t OMXCodec::setAPEFormat(const sp<MetaData> &meta)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    OMX_AUDIO_PARAM_APETYPE param;

    if (mIsEncoder) {
        CODEC_LOGE("APE encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
    CHECK(meta->findInt32(kKeySampleBits, &bitsPerSample));

    CODEC_LOGV("Channels:%d, SampleRate:%d, bitsPerSample:%d",
            numChannels, sampleRate, bitsPerSample);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioApe, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSamplingRate = sampleRate;
    param.nBitsPerSample = bitsPerSample;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioApe, &param, sizeof(param));
    return err;
}

status_t OMXCodec::setDTSFormat(const sp<MetaData> &meta)
{
    int32_t numChannels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    OMX_AUDIO_PARAM_DTSTYPE param;

    if (mIsEncoder) {
        CODEC_LOGE("DTS encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));

    CODEC_LOGV("Channels: %d, SampleRate: %d",
            numChannels, sampleRate);

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioDts, &param, sizeof(param));
    if (err != OK)
        return err;

    param.nChannels = numChannels;
    param.nSamplingRate = sampleRate;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioDts, &param, sizeof(param));
    return err;
}

status_t OMXCodec::setFFmpegAudioFormat(const sp<MetaData> &meta)
{
    int32_t codec_id = 0;
    int32_t numChannels = 0;
    int32_t bitRate = 0;
    int32_t bitsPerSample = 0;
    int32_t sampleRate = 0;
    int32_t blockAlign = 0;
    int32_t sampleFormat = 0;
    OMX_AUDIO_PARAM_FFMPEGTYPE param;

    ALOGD("setFFmpegAudioFormat");

    if (mIsEncoder) {
        CODEC_LOGE("FFMPEG encoding not supported");
        return OK;
    }

    CHECK(meta->findInt32(kKeyCodecId, &codec_id));
    CHECK(meta->findInt32(kKeyChannelCount, &numChannels));
    CHECK(meta->findInt32(kKeyBitRate, &bitRate));
    CHECK(meta->findInt32(kKeySampleBits, &bitsPerSample));
    CHECK(meta->findInt32(kKeySampleRate, &sampleRate));
    CHECK(meta->findInt32(kKeyBlockAlign, &blockAlign));
    CHECK(meta->findInt32(kKeySampleFormat, &sampleFormat));

    InitOMXParams(&param);
    param.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioFFmpeg, &param, sizeof(param));
    if (err != OK)
        return err;

    param.eCodecId       = codec_id;
    param.nChannels      = numChannels;
    param.nBitRate       = bitRate;
    param.nBitsPerSample = bitsPerSample;
    param.nSampleRate    = sampleRate;
    param.nBlockAlign    = blockAlign;
    param.eSampleFormat  = sampleFormat;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioFFmpeg, &param, sizeof(param));

    return err;
}

void OMXCodec::setG711Format(int32_t numChannels) {
    CHECK(!mIsEncoder);
    setRawAudioFormat(kPortIndexInput, 8000, numChannels);
}

void OMXCodec::setImageOutputFormat(
        OMX_COLOR_FORMATTYPE format, OMX_U32 width, OMX_U32 height) {
    CODEC_LOGV("setImageOutputFormat(%ld, %ld)", width, height);

#if 0
    OMX_INDEXTYPE index;
    status_t err = mOMX->get_extension_index(
            mNode, "OMX.TI.JPEG.decode.Config.OutputColorFormat", &index);
    CHECK_EQ(err, (status_t)OK);

    err = mOMX->set_config(mNode, index, &format, sizeof(format));
    CHECK_EQ(err, (status_t)OK);
#endif

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    CHECK_EQ((int)def.eDomain, (int)OMX_PortDomainImage);

    OMX_IMAGE_PORTDEFINITIONTYPE *imageDef = &def.format.image;

    CHECK_EQ((int)imageDef->eCompressionFormat, (int)OMX_IMAGE_CodingUnused);
    imageDef->eColorFormat = format;
    imageDef->nFrameWidth = width;
    imageDef->nFrameHeight = height;

    switch (format) {
        case OMX_COLOR_FormatYUV420PackedPlanar:
        case OMX_COLOR_FormatYUV411Planar:
        {
            def.nBufferSize = (width * height * 3) / 2;
            break;
        }

        case OMX_COLOR_FormatCbYCrY:
        {
            def.nBufferSize = width * height * 2;
            break;
        }

        case OMX_COLOR_Format32bitARGB8888:
        {
            def.nBufferSize = width * height * 4;
            break;
        }

        case OMX_COLOR_Format16bitARGB4444:
        case OMX_COLOR_Format16bitARGB1555:
        case OMX_COLOR_Format16bitRGB565:
        case OMX_COLOR_Format16bitBGR565:
        {
            def.nBufferSize = width * height * 2;
            break;
        }

        default:
            CHECK(!"Should not be here. Unknown color format.");
            break;
    }

    def.nBufferCountActual = def.nBufferCountMin;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);
}

void OMXCodec::setJPEGInputFormat(
        OMX_U32 width, OMX_U32 height, OMX_U32 compressedSize) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexInput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    CHECK_EQ((int)def.eDomain, (int)OMX_PortDomainImage);
    OMX_IMAGE_PORTDEFINITIONTYPE *imageDef = &def.format.image;

    CHECK_EQ((int)imageDef->eCompressionFormat, (int)OMX_IMAGE_CodingJPEG);
    imageDef->nFrameWidth = width;
    imageDef->nFrameHeight = height;

    def.nBufferSize = compressedSize;
    def.nBufferCountActual = def.nBufferCountMin;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);
}

void OMXCodec::addCodecSpecificData(const void *data, size_t size) {
    CodecSpecificData *specific =
        (CodecSpecificData *)malloc(sizeof(CodecSpecificData) + size - 1);

    specific->mSize = size;
    memcpy(specific->mData, data, size);

    mCodecSpecificData.push(specific);
}

void OMXCodec::clearCodecSpecificData() {
    for (size_t i = 0; i < mCodecSpecificData.size(); ++i) {
        free(mCodecSpecificData.editItemAt(i));
    }
    mCodecSpecificData.clear();
    mCodecSpecificDataIndex = 0;
}

status_t OMXCodec::start(MetaData *meta) {
    Mutex::Autolock autoLock(mLock);

    if(mPaused && mIsEncoder) {
        CODEC_LOGV("resume : S");
        //wake waitForBufferFilled_l() to avoid timeout when mPause becomes false
        mBufferFilled.signal();
        mPaused = false;

        if (mIsVideo) {
            status_t err = OMX_ErrorNone;
            OMX_CONFIG_INTRAREFRESHVOPTYPE vop;
            InitOMXParams(&vop);
            vop.nPortIndex = kPortIndexOutput; // output
            {
                vop.IntraRefreshVOP = OMX_TRUE;
                err = mOMX->setConfig(mNode,
                            OMX_IndexConfigVideoIntraVOPRefresh,
                            &vop,sizeof(vop));
                if (err != OMX_ErrorNone) {
                    CODEC_LOGE("I frame Request failed");
                }
            }
        }

        drainInputBuffers();
        return OK;
    }

    if (mPaused) {
        status_t err = resumeLocked(true);
        return err;
    }

    if (mState != LOADED) {
        CODEC_LOGE("called start in the unexpected state: %d", mState);
        return UNKNOWN_ERROR;
    }

    sp<MetaData> params = new MetaData;
    if (mQuirks & kWantsNALFragments) {
        params->setInt32(kKeyWantsNALFragments, true);
    }
    if (meta) {
        int64_t startTimeUs = 0;
        int64_t timeUs;
        if (meta->findInt64(kKeyTime, &timeUs)) {
            startTimeUs = timeUs;
        }
        params->setInt64(kKeyTime, startTimeUs);
    }

    mCodecSpecificDataIndex = 0;
    mInitialBufferSubmit = true;
    mSignalledEOS = false;
    mNoMoreOutputData = false;
    mOutputPortSettingsHaveChanged = false;
    mSeekTimeUs = -1;
    mSeekMode = ReadOptions::SEEK_CLOSEST_SYNC;
    mTargetTimeUs = -1;
    mFilledBuffers.clear();
    mPaused = false;
    mSignalledReadTryAgain = false;
    mReturnedRetry = false;
    mLastSeekTimeUs = -1;
    mLastSeekMode = ReadOptions::SEEK_CLOSEST;

    status_t err;
    if (mIsEncoder) {
        // Calling init() before starting its source so that we can configure,
        // if supported, the source to use exactly the same number of input
        // buffers as requested by the encoder.
        if ((err = init()) != OK) {
            CODEC_LOGE("init failed: %d", err);
            return err;
        }

        params->setInt32(kKeyNumBuffers, mPortBuffers[kPortIndexInput].size());
        err = mSource->start(params.get());
        if (err != OK) {
            CODEC_LOGE("source failed to start: %d", err);
            stopOmxComponent_l();
        }
        return err;
    }

    // Decoder case
    if ((err = mSource->start(params.get())) != OK) {
        CODEC_LOGE("source failed to start: %d", err);
        return err;
    }
    if ((err = init()) != OK) {
        CODEC_LOGE("init failed: %d", err);
        //Something went wrong..component refused to move to idle or allocation
        //failed. Set state to error and force stopping component to cleanup as
        //much as possible
        setState(ERROR);
        stopOmxComponent_l();
    }
    return err;
}

status_t OMXCodec::stop() {
    CODEC_LOGV("stop mState=%d", mState);
    Mutex::Autolock autoLock(mLock);
    status_t err = stopOmxComponent_l();
    mSource->stop();

    CODEC_LOGV("stopped in state %d", mState);
    return err;
}

status_t OMXCodec::stopOmxComponent_l() {
    CODEC_LOGV("stopOmxComponent_l mState=%d", mState);

    while (isIntermediateState(mState)) {
        mAsyncCompletion.wait(mLock);
    }

    bool isError = false;
    bool forceFlush = false;
    switch (mState) {
        case LOADED:
            break;

        case ERROR:
        {
            if (mPortStatus[kPortIndexOutput] == ENABLING) {
                // Codec is in a wedged state (technical term)
                // We've seen an output port settings change from the codec,
                // We've disabled the output port, then freed the output
                // buffers, initiated re-enabling the output port but
                // failed to reallocate the output buffers.
                // There doesn't seem to be a way to orderly transition
                // from executing->idle and idle->loaded now that the
                // output port hasn't been reenabled yet...
                // Simply free as many resources as we can and pretend
                // that we're in LOADED state so that the destructor
                // will free the component instance without asserting.
                freeBuffersOnPort(kPortIndexInput, true /* onlyThoseWeOwn */);
                freeBuffersOnPort(kPortIndexOutput, true /* onlyThoseWeOwn */);
                setState(LOADED);
                break;
            } else {
                OMX_STATETYPE state = OMX_StateInvalid;
                status_t err = mOMX->getState(mNode, &state);
                CHECK_EQ(err, (status_t)OK);

                if (state != OMX_StateExecuting) {
                    forceFlush = true;
                    break;
                }
                // else fall through to the idling code
            }

            isError = true;
        }

        case PAUSED:
        case EXECUTING:
        {
            setState(EXECUTING_TO_IDLE);

            if (mQuirks & kRequiresFlushBeforeShutdown) {
                CODEC_LOGV("This component requires a flush before transitioning "
                     "from EXECUTING to IDLE...");

                //DSP supports flushing of ports simultaneously.
                //Flushing individual port is not supported.
                if(mQuirks & kRequiresGlobalFlush) {
                    bool emulateFlushCompletion = !flushPortAsync(kPortIndexBoth);
                    if (emulateFlushCompletion) {
                        onCmdComplete(OMX_CommandFlush, kPortIndexBoth);
                    }
                } else {
                    bool emulateInputFlushCompletion =
                        !flushPortAsync(kPortIndexInput);

                    bool emulateOutputFlushCompletion =
                        !flushPortAsync(kPortIndexOutput);

                    if (emulateInputFlushCompletion) {
                        onCmdComplete(OMX_CommandFlush, kPortIndexInput);
                    }

                    if (emulateOutputFlushCompletion) {
                        onCmdComplete(OMX_CommandFlush, kPortIndexOutput);
                    }
                }
            } else {
                mPortStatus[kPortIndexInput] = SHUTTING_DOWN;
                mPortStatus[kPortIndexOutput] = SHUTTING_DOWN;

                status_t err =
                    mOMX->sendCommand(mNode, OMX_CommandStateSet, OMX_StateIdle);
                CHECK_EQ(err, (status_t)OK);
            }

            while (mState != LOADED && mState != ERROR) {
                mAsyncCompletion.wait(mLock);
            }

            if (mState == ERROR) {
                forceFlush = true;
            }

            if (isError) {
                // We were in the ERROR state coming in, so restore that now
                // that we've idled the OMX component.
                setState(ERROR);
            }

            break;
        }

        default:
        {
            CHECK(!"should not be here.");
            break;
        }
    }

    if (forceFlush) {
        flushBuffersOnError();
    }

    if (mLeftOverBuffer) {
        mLeftOverBuffer->release();
        mLeftOverBuffer = NULL;
    }

    return OK;
}

sp<MetaData> OMXCodec::getFormat() {
    Mutex::Autolock autoLock(mLock);

    return mOutputFormat;
}

status_t OMXCodec::read(
        MediaBuffer **buffer, const ReadOptions *options) {
    ATRACE_CALL();
    status_t err = OK;
    *buffer = NULL;

    Mutex::Autolock autoLock(mLock);

    if (mPaused && !mIsEncoder) {
        err = resumeLocked(false);
        if(err != OK) {
            CODEC_LOGE("Failed to restart codec err= %d", err);
            return err;
        }
    }

    if (mState != EXECUTING && mState != RECONFIGURING) {
        if(mState == FLUSHING) {
            mReturnedRetry = true;
            return -EAGAIN;
        }
        mReturnedRetry = false;
        return UNKNOWN_ERROR;
    }

    bool seeking = false;
    int64_t seekTimeUs = -1;
    ReadOptions::SeekMode seekMode = ReadOptions::SEEK_CLOSEST;
    if (options && options->getSeekTo(&seekTimeUs, &seekMode)) {
        seeking = true;
        if(mReturnedRetry &&
          (seekTimeUs == mLastSeekTimeUs) &&
          (seekMode == mLastSeekMode))
            seeking = false;
        mLastSeekTimeUs = seekTimeUs;
        mLastSeekMode = seekMode;
    }

    if (mInitialBufferSubmit) {
        mInitialBufferSubmit = false;

        if (seeking) {
            CHECK(seekTimeUs >= 0);
            mSeekTimeUs = seekTimeUs;
            mSeekMode = seekMode;

            // There's no reason to trigger the code below, there's
            // nothing to flush yet.
            seeking = false;
            mPaused = false;
        }

        if (mQuirks & kRequiresFlushCompleteEmulation)
            drainInputBuffers();

        if (mState == EXECUTING) {
            // Otherwise mState == RECONFIGURING and this code will trigger
            // after the output port is reenabled.
            fillOutputBuffers();
        }
        if (!(mQuirks & kRequiresFlushCompleteEmulation))
            drainInputBuffers();
    }

    if (seeking) {
        while (mState == RECONFIGURING) {
            if ((err = waitForBufferFilled_l()) != OK) {
                mReturnedRetry = (err == -EAGAIN);
                return err;
            }
        }

        if (mState != EXECUTING) {
            mReturnedRetry = false;
            return UNKNOWN_ERROR;
        }

        CODEC_LOGV("seeking to %lld us (%.2f secs)", seekTimeUs, seekTimeUs / 1E6);

        mSignalledEOS = false;

        CHECK(seekTimeUs >= 0);
        mSeekTimeUs = seekTimeUs;
        mSeekMode = seekMode;

        mFilledBuffers.clear();

        CHECK_EQ((int)mState, (int)EXECUTING);
        setState(FLUSHING);

        //DSP supports flushing of ports simultaneously.
        //Flushing individual port is not supported.
        if(mQuirks & kRequiresGlobalFlush) {
            bool emulateFlushCompletion = !flushPortAsync(kPortIndexBoth);
            if (emulateFlushCompletion) {
                onCmdComplete(OMX_CommandFlush, kPortIndexBoth);
            }
        } else {
            bool emulateInputFlushCompletion = !flushPortAsync(kPortIndexInput);
            bool emulateOutputFlushCompletion = !flushPortAsync(kPortIndexOutput);

            if (emulateInputFlushCompletion) {
                onCmdComplete(OMX_CommandFlush, kPortIndexInput);
            }

            if (emulateOutputFlushCompletion) {
                onCmdComplete(OMX_CommandFlush, kPortIndexOutput);
            }
        }

        while (mSeekTimeUs >= 0) {
            if ((err = waitForBufferFilled_l()) != OK) {
                mReturnedRetry = (err == -EAGAIN);
                return err;
            }
        }
    }

    if ((mSignalledReadTryAgain == true) && (mState == EXECUTING)) {
        drainInputBuffers();
    }

    if (!strncasecmp(mMIME, "video/", 6)) {
        ATRACE_INT("Output buffers with OMXCodec", mFilledBuffers.size());
        ATRACE_INT("Output Buffers with OMX client",
                countBuffersWeOwn(mPortBuffers[kPortIndexOutput]));
    }

    while (mState != ERROR && !mNoMoreOutputData && mFilledBuffers.empty() &&
           !mOutputPortSettingsChangedPending) {
        if ((err = waitForBufferFilled_l()) != OK) {
            mReturnedRetry = (err == -EAGAIN);
            return err;
        }
    }
    mReturnedRetry = false;
    if (mState == ERROR) {
        return UNKNOWN_ERROR;
    }

    if (mFilledBuffers.empty()) {
        return mSignalledEOS ? mFinalStatus : ERROR_END_OF_STREAM;
    }

    if (mOutputPortSettingsHaveChanged) {
        mOutputPortSettingsHaveChanged = false;

        return INFO_FORMAT_CHANGED;
    }

    size_t index = *mFilledBuffers.begin();
    mFilledBuffers.erase(mFilledBuffers.begin());
    if (!strncasecmp(mMIME, "video/", 6)) {
        ATRACE_INT("Output buffers with OMXCodec", mFilledBuffers.size());
        ATRACE_INT("Output Buffers with OMX client",
                countBuffersWeOwn(mPortBuffers[kPortIndexOutput]));
    }

    BufferInfo *info = &mPortBuffers[kPortIndexOutput].editItemAt(index);
    CHECK_EQ((int)info->mStatus, (int)OWNED_BY_US);
    info->mStatus = OWNED_BY_CLIENT;

    info->mMediaBuffer->add_ref();
    if (mSkipCutBuffer != NULL) {
        mSkipCutBuffer->submit(info->mMediaBuffer);
    }
    *buffer = info->mMediaBuffer;

    if (info->mOutputCropChanged) {
        initNativeWindowCrop();
        info->mOutputCropChanged = false;
    }
    return OK;
}

void OMXCodec::signalBufferReturned(MediaBuffer *buffer) {
    Mutex::Autolock autoLock(mLock);

    Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexOutput];
    for (size_t i = 0; i < buffers->size(); ++i) {
        BufferInfo *info = &buffers->editItemAt(i);

        if (info->mMediaBuffer == buffer) {
            CHECK_EQ((int)mPortStatus[kPortIndexOutput], (int)ENABLED);
            CHECK_EQ((int)info->mStatus, (int)OWNED_BY_CLIENT);

            info->mStatus = OWNED_BY_US;

            if (buffer->graphicBuffer() == 0) {
                fillOutputBuffer(info);
            } else {
                sp<MetaData> metaData = info->mMediaBuffer->meta_data();
                int32_t rendered = 0;
                if (!metaData->findInt32(kKeyRendered, &rendered)) {
                    rendered = 0;
                }
                if (!rendered) {
                    status_t err = cancelBufferToNativeWindow(info);
                    if (err < 0) {
                        return;
                    }
                }

                info->mStatus = OWNED_BY_NATIVE_WINDOW;

                // Dequeue the next buffer from the native window.
                BufferInfo *nextBufInfo = dequeueBufferFromNativeWindow();
                if (nextBufInfo == 0) {
                    return;
                }

                // Give the buffer to the OMX node to fill.
                fillOutputBuffer(nextBufInfo);
            }
            return;
        }
    }

    CHECK(!"should not be here.");
}

static const char *imageCompressionFormatString(OMX_IMAGE_CODINGTYPE type) {
    static const char *kNames[] = {
        "OMX_IMAGE_CodingUnused",
        "OMX_IMAGE_CodingAutoDetect",
        "OMX_IMAGE_CodingJPEG",
        "OMX_IMAGE_CodingJPEG2K",
        "OMX_IMAGE_CodingEXIF",
        "OMX_IMAGE_CodingTIFF",
        "OMX_IMAGE_CodingGIF",
        "OMX_IMAGE_CodingPNG",
        "OMX_IMAGE_CodingLZW",
        "OMX_IMAGE_CodingBMP",
    };

    size_t numNames = sizeof(kNames) / sizeof(kNames[0]);

    if (type < 0 || (size_t)type >= numNames) {
        return "UNKNOWN";
    } else {
        return kNames[type];
    }
}

static const char *colorFormatString(OMX_COLOR_FORMATTYPE type) {
    static const char *kNames[] = {
        "OMX_COLOR_FormatUnused",
        "OMX_COLOR_FormatMonochrome",
        "OMX_COLOR_Format8bitRGB332",
        "OMX_COLOR_Format12bitRGB444",
        "OMX_COLOR_Format16bitARGB4444",
        "OMX_COLOR_Format16bitARGB1555",
        "OMX_COLOR_Format16bitRGB565",
        "OMX_COLOR_Format16bitBGR565",
        "OMX_COLOR_Format18bitRGB666",
        "OMX_COLOR_Format18bitARGB1665",
        "OMX_COLOR_Format19bitARGB1666",
        "OMX_COLOR_Format24bitRGB888",
        "OMX_COLOR_Format24bitBGR888",
        "OMX_COLOR_Format24bitARGB1887",
        "OMX_COLOR_Format25bitARGB1888",
        "OMX_COLOR_Format32bitBGRA8888",
        "OMX_COLOR_Format32bitARGB8888",
        "OMX_COLOR_FormatYUV411Planar",
        "OMX_COLOR_FormatYUV411PackedPlanar",
        "OMX_COLOR_FormatYUV420Planar",
        "OMX_COLOR_FormatYUV420PackedPlanar",
        "OMX_COLOR_FormatYUV420SemiPlanar",
        "OMX_COLOR_FormatYUV422Planar",
        "OMX_COLOR_FormatYUV422PackedPlanar",
        "OMX_COLOR_FormatYUV422SemiPlanar",
        "OMX_COLOR_FormatYCbYCr",
        "OMX_COLOR_FormatYCrYCb",
        "OMX_COLOR_FormatCbYCrY",
        "OMX_COLOR_FormatCrYCbY",
        "OMX_COLOR_FormatYUV444Interleaved",
        "OMX_COLOR_FormatRawBayer8bit",
        "OMX_COLOR_FormatRawBayer10bit",
        "OMX_COLOR_FormatRawBayer8bitcompressed",
        "OMX_COLOR_FormatL2",
        "OMX_COLOR_FormatL4",
        "OMX_COLOR_FormatL8",
        "OMX_COLOR_FormatL16",
        "OMX_COLOR_FormatL24",
        "OMX_COLOR_FormatL32",
        "OMX_COLOR_FormatYUV420PackedSemiPlanar",
        "OMX_COLOR_FormatYUV422PackedSemiPlanar",
        "OMX_COLOR_Format18BitBGR666",
        "OMX_COLOR_Format24BitARGB6666",
        "OMX_COLOR_Format24BitABGR6666",
    };

    size_t numNames = sizeof(kNames) / sizeof(kNames[0]);

    if (type == OMX_TI_COLOR_FormatYUV420PackedSemiPlanar) {
        return "OMX_TI_COLOR_FormatYUV420PackedSemiPlanar";
    }
#ifdef USE_SAMSUNG_COLORFORMAT
    if (type == OMX_SEC_COLOR_FormatNV12TPhysicalAddress) {
        return "OMX_SEC_COLOR_FormatNV12TPhysicalAddress";
    }
    if (type == OMX_SEC_COLOR_FormatNV12LPhysicalAddress) {
        return "OMX_SEC_COLOR_FormatNV12LPhysicalAddress";
    }
    if (type == OMX_SEC_COLOR_FormatNV12LVirtualAddress) {
        return "OMX_SEC_COLOR_FormatNV12LVirtualAddress";
    }
    if (type == OMX_SEC_COLOR_FormatNV12Tiled) {
        return "OMX_SEC_COLOR_FormatNV12Tiled";
    }
#endif // USE_SAMSUNG_COLORFORMAT
    else if (type == OMX_QCOM_COLOR_FormatYVU420SemiPlanar) {
        return "OMX_QCOM_COLOR_FormatYVU420SemiPlanar";
    } else if (type < 0 || (size_t)type >= numNames) {
        return "UNKNOWN";
    } else {
        return kNames[type];
    }
}

static const char *vendorVideoCompressionFormatString(OMX_VIDEO_CODINGTYPE type) {
    static const char *kVendorNames[] = {
        "OMX_VIDEO_CodingVendorStartUnused",
        "OMX_VIDEO_CodingVC1",
        "OMX_VIDEO_CodingFLV1",
        "OMX_VIDEO_CodingDIVX",
        "OMX_VIDEO_CodingHEVC",
        "OMX_VIDEO_CodingFFMPEG",
    };

    CHECK_GE(type, OMX_VIDEO_CodingVendorStartUnused);

    size_t index = (size_t)type - (size_t)OMX_VIDEO_CodingVendorStartUnused;

    size_t numNames = sizeof(kVendorNames) / sizeof(kVendorNames[0]);

    if (index >= numNames) {
        return "UNKNOWN";
    } else {
        return kVendorNames[index];
    }
}

static const char *videoCompressionFormatString(OMX_VIDEO_CODINGTYPE type) {
    static const char *kNames[] = {
        "OMX_VIDEO_CodingUnused",
        "OMX_VIDEO_CodingAutoDetect",
        "OMX_VIDEO_CodingMPEG2",
        "OMX_VIDEO_CodingH263",
        "OMX_VIDEO_CodingMPEG4",
        "OMX_VIDEO_CodingWMV",
        "OMX_VIDEO_CodingRV",
        "OMX_VIDEO_CodingAVC",
        "OMX_VIDEO_CodingMJPEG",
        "OMX_VIDEO_CodingVPX",
    };

    if (type >= OMX_VIDEO_CodingVendorStartUnused) {
        return vendorVideoCompressionFormatString(type);
    }

    size_t numNames = sizeof(kNames) / sizeof(kNames[0]);

    if (type < 0 || (size_t)type >= numNames) {
        return "UNKNOWN";
    } else {
        return kNames[type];
    }
}

static const char *vendorAudioCodingTypeString(OMX_AUDIO_CODINGTYPE type) {
    static const char *kVendorNames[] = {
        "OMX_AUDIO_CodingVendorStartUnused",
        "OMX_AUDIO_CodingMP2",
        "OMX_AUDIO_CodingAC3",
        "OMX_AUDIO_CodingAPE",
        "OMX_AUDIO_CodingDTS",
        "OMX_AUDIO_CodingFFMPEG",
    };

    CHECK_GE(type, OMX_AUDIO_CodingVendorStartUnused);

    size_t index = (size_t)type - (size_t)OMX_AUDIO_CodingVendorStartUnused;

    size_t numNames = sizeof(kVendorNames) / sizeof(kVendorNames[0]);

    if (index >= numNames) {
        return "UNKNOWN";
    } else {
        return kVendorNames[index];
    }
}

static const char *audioCodingTypeString(OMX_AUDIO_CODINGTYPE type) {
    static const char *kNames[] = {
        "OMX_AUDIO_CodingUnused",
        "OMX_AUDIO_CodingAutoDetect",
        "OMX_AUDIO_CodingPCM",
        "OMX_AUDIO_CodingADPCM",
        "OMX_AUDIO_CodingAMR",
        "OMX_AUDIO_CodingGSMFR",
        "OMX_AUDIO_CodingGSMEFR",
        "OMX_AUDIO_CodingGSMHR",
        "OMX_AUDIO_CodingPDCFR",
        "OMX_AUDIO_CodingPDCEFR",
        "OMX_AUDIO_CodingPDCHR",
        "OMX_AUDIO_CodingTDMAFR",
        "OMX_AUDIO_CodingTDMAEFR",
        "OMX_AUDIO_CodingQCELP8",
        "OMX_AUDIO_CodingQCELP13",
        "OMX_AUDIO_CodingEVRC",
        "OMX_AUDIO_CodingSMV",
        "OMX_AUDIO_CodingG711",
        "OMX_AUDIO_CodingG723",
        "OMX_AUDIO_CodingG726",
        "OMX_AUDIO_CodingG729",
        "OMX_AUDIO_CodingAAC",
        "OMX_AUDIO_CodingMP3",
        "OMX_AUDIO_CodingSBC",
        "OMX_AUDIO_CodingVORBIS",
        "OMX_AUDIO_CodingWMA",
        "OMX_AUDIO_CodingRA",
        "OMX_AUDIO_CodingMIDI",
        "OMX_AUDIO_CodingFLAC",
#ifdef DOLBY_UDC
        "OMX_AUDIO_CodingDDP",
#endif // DOLBY_UDC
    };

    if (type >= OMX_AUDIO_CodingVendorStartUnused) {
        return vendorAudioCodingTypeString(type);
    }

    size_t numNames = sizeof(kNames) / sizeof(kNames[0]);

    if (type < 0 || (size_t)type >= numNames) {
        return "UNKNOWN";
    } else {
        return kNames[type];
    }
}

static const char *audioPCMModeString(OMX_AUDIO_PCMMODETYPE type) {
    static const char *kNames[] = {
        "OMX_AUDIO_PCMModeLinear",
        "OMX_AUDIO_PCMModeALaw",
        "OMX_AUDIO_PCMModeMULaw",
    };

    size_t numNames = sizeof(kNames) / sizeof(kNames[0]);

    if (type < 0 || (size_t)type >= numNames) {
        return "UNKNOWN";
    } else {
        return kNames[type];
    }
}

static const char *amrBandModeString(OMX_AUDIO_AMRBANDMODETYPE type) {
    static const char *kNames[] = {
        "OMX_AUDIO_AMRBandModeUnused",
        "OMX_AUDIO_AMRBandModeNB0",
        "OMX_AUDIO_AMRBandModeNB1",
        "OMX_AUDIO_AMRBandModeNB2",
        "OMX_AUDIO_AMRBandModeNB3",
        "OMX_AUDIO_AMRBandModeNB4",
        "OMX_AUDIO_AMRBandModeNB5",
        "OMX_AUDIO_AMRBandModeNB6",
        "OMX_AUDIO_AMRBandModeNB7",
        "OMX_AUDIO_AMRBandModeWB0",
        "OMX_AUDIO_AMRBandModeWB1",
        "OMX_AUDIO_AMRBandModeWB2",
        "OMX_AUDIO_AMRBandModeWB3",
        "OMX_AUDIO_AMRBandModeWB4",
        "OMX_AUDIO_AMRBandModeWB5",
        "OMX_AUDIO_AMRBandModeWB6",
        "OMX_AUDIO_AMRBandModeWB7",
        "OMX_AUDIO_AMRBandModeWB8",
    };

    size_t numNames = sizeof(kNames) / sizeof(kNames[0]);

    if (type < 0 || (size_t)type >= numNames) {
        return "UNKNOWN";
    } else {
        return kNames[type];
    }
}

static const char *amrFrameFormatString(OMX_AUDIO_AMRFRAMEFORMATTYPE type) {
    static const char *kNames[] = {
        "OMX_AUDIO_AMRFrameFormatConformance",
        "OMX_AUDIO_AMRFrameFormatIF1",
        "OMX_AUDIO_AMRFrameFormatIF2",
        "OMX_AUDIO_AMRFrameFormatFSF",
        "OMX_AUDIO_AMRFrameFormatRTPPayload",
        "OMX_AUDIO_AMRFrameFormatITU",
    };

    size_t numNames = sizeof(kNames) / sizeof(kNames[0]);

    if (type < 0 || (size_t)type >= numNames) {
        return "UNKNOWN";
    } else {
        return kNames[type];
    }
}

void OMXCodec::dumpPortStatus(OMX_U32 portIndex) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    printf("%s Port = {\n", portIndex == kPortIndexInput ? "Input" : "Output");

    CHECK((portIndex == kPortIndexInput && def.eDir == OMX_DirInput)
          || (portIndex == kPortIndexOutput && def.eDir == OMX_DirOutput));

    printf("  nBufferCountActual = %ld\n", def.nBufferCountActual);
    printf("  nBufferCountMin = %ld\n", def.nBufferCountMin);
    printf("  nBufferSize = %ld\n", def.nBufferSize);

    switch (def.eDomain) {
        case OMX_PortDomainImage:
        {
            const OMX_IMAGE_PORTDEFINITIONTYPE *imageDef = &def.format.image;

            printf("\n");
            printf("  // Image\n");
            printf("  nFrameWidth = %ld\n", imageDef->nFrameWidth);
            printf("  nFrameHeight = %ld\n", imageDef->nFrameHeight);
            printf("  nStride = %ld\n", imageDef->nStride);

            printf("  eCompressionFormat = %s\n",
                   imageCompressionFormatString(imageDef->eCompressionFormat));

            printf("  eColorFormat = %s\n",
                   colorFormatString(imageDef->eColorFormat));

            break;
        }

        case OMX_PortDomainVideo:
        {
            OMX_VIDEO_PORTDEFINITIONTYPE *videoDef = &def.format.video;

            printf("\n");
            printf("  // Video\n");
            printf("  nFrameWidth = %ld\n", videoDef->nFrameWidth);
            printf("  nFrameHeight = %ld\n", videoDef->nFrameHeight);
            printf("  nStride = %ld\n", videoDef->nStride);

            printf("  eCompressionFormat = %s\n",
                   videoCompressionFormatString(videoDef->eCompressionFormat));

            printf("  eColorFormat = %s\n",
                   colorFormatString(videoDef->eColorFormat));

            break;
        }

        case OMX_PortDomainAudio:
        {
            OMX_AUDIO_PORTDEFINITIONTYPE *audioDef = &def.format.audio;

            printf("\n");
            printf("  // Audio\n");
            printf("  eEncoding = %s\n",
                   audioCodingTypeString(audioDef->eEncoding));

            if (audioDef->eEncoding == OMX_AUDIO_CodingPCM) {
                OMX_AUDIO_PARAM_PCMMODETYPE params;
                InitOMXParams(&params);
                params.nPortIndex = portIndex;

                err = mOMX->getParameter(
                        mNode, OMX_IndexParamAudioPcm, &params, sizeof(params));
                CHECK_EQ(err, (status_t)OK);

                printf("  nSamplingRate = %ld\n", params.nSamplingRate);
                printf("  nChannels = %ld\n", params.nChannels);
                printf("  bInterleaved = %d\n", params.bInterleaved);
                printf("  nBitPerSample = %ld\n", params.nBitPerSample);

                printf("  eNumData = %s\n",
                       params.eNumData == OMX_NumericalDataSigned
                        ? "signed" : "unsigned");

                printf("  ePCMMode = %s\n", audioPCMModeString(params.ePCMMode));
            } else if (audioDef->eEncoding == OMX_AUDIO_CodingAMR) {
                OMX_AUDIO_PARAM_AMRTYPE amr;
                InitOMXParams(&amr);
                amr.nPortIndex = portIndex;

                err = mOMX->getParameter(
                        mNode, OMX_IndexParamAudioAmr, &amr, sizeof(amr));
                CHECK_EQ(err, (status_t)OK);

                printf("  nChannels = %ld\n", amr.nChannels);
                printf("  eAMRBandMode = %s\n",
                        amrBandModeString(amr.eAMRBandMode));
                printf("  eAMRFrameFormat = %s\n",
                        amrFrameFormatString(amr.eAMRFrameFormat));
            }

            break;
        }

        default:
        {
            printf("  // Unknown\n");
            break;
        }
    }

    printf("}\n");
}

status_t OMXCodec::initNativeWindow() {
    // Enable use of a GraphicBuffer as the output for this node.  This must
    // happen before getting the IndexParamPortDefinition parameter because it
    // will affect the pixel format that the node reports.
    status_t err = mOMX->enableGraphicBuffers(mNode, kPortIndexOutput, OMX_TRUE);
    if (err != 0) {
        return err;
    }

    return OK;
}

void OMXCodec::initNativeWindowCrop() {
    int32_t left, top, right, bottom;

    CHECK(mOutputFormat->findRect(
                        kKeyCropRect,
                        &left, &top, &right, &bottom));

    android_native_rect_t crop;
    crop.left = left;
    crop.top = top;
    crop.right = right + 1;
    crop.bottom = bottom + 1;

    // We'll ignore any errors here, if the surface is
    // already invalid, we'll know soon enough.
    native_window_set_crop(mNativeWindow.get(), &crop);
}

void OMXCodec::initOutputFormat(const sp<MetaData> &inputFormat) {
    mOutputFormat = new MetaData;
    mOutputFormat->setCString(kKeyDecoderComponent, mComponentName);
    if (mIsEncoder) {
        int32_t timeScale;
        if (inputFormat->findInt32(kKeyTimeScale, &timeScale)) {
            mOutputFormat->setInt32(kKeyTimeScale, timeScale);
        }
    }

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));
    CHECK_EQ(err, (status_t)OK);

    switch (def.eDomain) {
        case OMX_PortDomainImage:
        {
            OMX_IMAGE_PORTDEFINITIONTYPE *imageDef = &def.format.image;
            CHECK_EQ((int)imageDef->eCompressionFormat,
                     (int)OMX_IMAGE_CodingUnused);

            mOutputFormat->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RAW);
            mOutputFormat->setInt32(kKeyColorFormat, imageDef->eColorFormat);
            mOutputFormat->setInt32(kKeyWidth, imageDef->nFrameWidth);
            mOutputFormat->setInt32(kKeyHeight, imageDef->nFrameHeight);
            break;
        }

        case OMX_PortDomainAudio:
        {
            OMX_AUDIO_PORTDEFINITIONTYPE *audio_def = &def.format.audio;

            if (audio_def->eEncoding == OMX_AUDIO_CodingPCM) {
                OMX_AUDIO_PARAM_PCMMODETYPE params;
                InitOMXParams(&params);
                params.nPortIndex = kPortIndexOutput;

                err = mOMX->getParameter(
                        mNode, OMX_IndexParamAudioPcm, &params, sizeof(params));
                CHECK_EQ(err, (status_t)OK);

                CHECK_EQ((int)params.eNumData, (int)OMX_NumericalDataSigned);
                CHECK_EQ(params.nBitPerSample, 16u);
                CHECK_EQ((int)params.ePCMMode, (int)OMX_AUDIO_PCMModeLinear);

                int32_t numChannels, sampleRate;
                inputFormat->findInt32(kKeyChannelCount, &numChannels);
                inputFormat->findInt32(kKeySampleRate, &sampleRate);

                if ((OMX_U32)numChannels != params.nChannels) {
                    ALOGI("Codec outputs a different number of channels than "
                         "the input stream contains (contains %d channels, "
                         "codec outputs %ld channels).",
                         numChannels, params.nChannels);
                }

                if (sampleRate != (int32_t)params.nSamplingRate) {
                    ALOGI("Codec outputs at different sampling rate than "
                         "what the input stream contains (contains data at "
                         "%d Hz, codec outputs %lu Hz)",
                         sampleRate, params.nSamplingRate);
                }

                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);

                // Use the codec-advertised number of channels, as some
                // codecs appear to output stereo even if the input data is
                // mono. If we know the codec lies about this information,
                // use the actual number of channels instead.
                mOutputFormat->setInt32(
                        kKeyChannelCount,
                        (mQuirks & kDecoderLiesAboutNumberOfChannels)
                            ? numChannels : params.nChannels);

                mOutputFormat->setInt32(kKeySampleRate, params.nSamplingRate);
            } else if (audio_def->eEncoding == OMX_AUDIO_CodingAMR) {
                OMX_AUDIO_PARAM_AMRTYPE amr;
                InitOMXParams(&amr);
                amr.nPortIndex = kPortIndexOutput;

                err = mOMX->getParameter(
                        mNode, OMX_IndexParamAudioAmr, &amr, sizeof(amr));
                CHECK_EQ(err, (status_t)OK);

                CHECK_EQ(amr.nChannels, 1u);
                mOutputFormat->setInt32(kKeyChannelCount, 1);

                if (amr.eAMRBandMode >= OMX_AUDIO_AMRBandModeNB0
                    && amr.eAMRBandMode <= OMX_AUDIO_AMRBandModeNB7) {
                    mOutputFormat->setCString(
                            kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AMR_NB);
                    mOutputFormat->setInt32(kKeySampleRate, 8000);
                } else if (amr.eAMRBandMode >= OMX_AUDIO_AMRBandModeWB0
                            && amr.eAMRBandMode <= OMX_AUDIO_AMRBandModeWB8) {
                    mOutputFormat->setCString(
                            kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AMR_WB);
                    mOutputFormat->setInt32(kKeySampleRate, 16000);
                } else {
                    CHECK(!"Unknown AMR band mode.");
                }
            } else if (audio_def->eEncoding == OMX_AUDIO_CodingAAC) {
                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AAC);
                int32_t numChannels, sampleRate, bitRate;
                inputFormat->findInt32(kKeyChannelCount, &numChannels);
                inputFormat->findInt32(kKeySampleRate, &sampleRate);
                inputFormat->findInt32(kKeyBitRate, &bitRate);
                mOutputFormat->setInt32(kKeyChannelCount, numChannels);
                mOutputFormat->setInt32(kKeySampleRate, sampleRate);
                mOutputFormat->setInt32(kKeyBitRate, bitRate);
            } else {
#ifdef QCOM_HARDWARE
                AString mimeType;
                if (OK == ExtendedCodec::handleSupportedAudioFormats(
                        audio_def->eEncoding, &mimeType)) {
                    mOutputFormat->setCString(
                            kKeyMIMEType, mimeType.c_str());
                    int32_t numChannels, sampleRate, bitRate;
                    inputFormat->findInt32(kKeyChannelCount, &numChannels);
                    inputFormat->findInt32(kKeySampleRate, &sampleRate);
                    inputFormat->findInt32(kKeyBitRate, &bitRate);
                    mOutputFormat->setInt32(kKeyChannelCount, numChannels);
                    mOutputFormat->setInt32(kKeySampleRate, sampleRate);
                    mOutputFormat->setInt32(kKeyBitRate, bitRate);
                } else {
#endif
                CHECK(!"Should not be here. Unknown audio encoding.");
#ifdef QCOM_HARDWARE
                }
#endif
            }
            break;
        }

        case OMX_PortDomainVideo:
        {
            OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;

            if (video_def->eCompressionFormat == OMX_VIDEO_CodingUnused) {
                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RAW);
            } else if (video_def->eCompressionFormat == OMX_VIDEO_CodingMPEG4) {
                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG4);
            } else if (video_def->eCompressionFormat == OMX_VIDEO_CodingH263) {
                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_H263);
            } else if (video_def->eCompressionFormat == OMX_VIDEO_CodingAVC) {
                mOutputFormat->setCString(
                        kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_AVC);
            } else {
                CHECK(!"Unknown compression format.");
            }

            mOutputFormat->setInt32(kKeyWidth, video_def->nFrameWidth);
            mOutputFormat->setInt32(kKeyHeight, video_def->nFrameHeight);
            mOutputFormat->setInt32(kKeyColorFormat, video_def->eColorFormat);

            if (!mIsEncoder) {
                OMX_CONFIG_RECTTYPE rect;
                InitOMXParams(&rect);
                rect.nPortIndex = kPortIndexOutput;
                status_t err =
                        mOMX->getConfig(
                            mNode, OMX_IndexConfigCommonOutputCrop,
                            &rect, sizeof(rect));

                CODEC_LOGI(
                        "video dimensions are %ld x %ld",
                        video_def->nFrameWidth, video_def->nFrameHeight);

                if (err == OK) {
                    CHECK_GE(rect.nLeft, 0);
                    CHECK_GE(rect.nTop, 0);
                    CHECK_GE(rect.nWidth, 0u);
                    CHECK_GE(rect.nHeight, 0u);
                    CHECK_LE(rect.nLeft + rect.nWidth - 1, video_def->nFrameWidth);
                    CHECK_LE(rect.nTop + rect.nHeight - 1, video_def->nFrameHeight);

                    mOutputFormat->setRect(
                            kKeyCropRect,
                            rect.nLeft,
                            rect.nTop,
                            rect.nLeft + rect.nWidth - 1,
                            rect.nTop + rect.nHeight - 1);

                    CODEC_LOGI(
                            "Crop rect is %ld x %ld @ (%ld, %ld)",
                            rect.nWidth, rect.nHeight, rect.nLeft, rect.nTop);
                } else {
                    mOutputFormat->setRect(
                            kKeyCropRect,
                            0, 0,
                            video_def->nFrameWidth - 1,
                            video_def->nFrameHeight - 1);
                }

                if (mNativeWindow != NULL) {
                     if (mInSmoothStreamingMode) {
                         mOutputCropChanged = true;
                     } else {
                         initNativeWindowCrop();
                     }
                }
#ifdef QCOM_HARDWARE
            } else {
                ExtendedUtils::HFR::copyHFRParams(inputFormat, mOutputFormat);
#endif
            }
            break;
        }

        default:
        {
            CHECK(!"should not be here, neither audio nor video.");
            break;
        }
    }

    // If the input format contains rotation information, flag the output
    // format accordingly.

    int32_t rotationDegrees;
    if (mSource->getFormat()->findInt32(kKeyRotation, &rotationDegrees)) {
        mOutputFormat->setInt32(kKeyRotation, rotationDegrees);
    }
}

status_t OMXCodec::pause() {
    CODEC_LOGV("pause mState=%d", mState);

    Mutex::Autolock autoLock(mLock);
    if (!strncmp(mComponentName, "OMX.qcom.", 9) && !mIsEncoder) {
        if (mState != EXECUTING) {
            return UNKNOWN_ERROR;
        }
        while (isIntermediateState(mState)) {
            mAsyncCompletion.wait(mLock);
        }

        status_t err = mOMX->sendCommand(mNode,
            OMX_CommandStateSet, OMX_StatePause);
        CHECK_EQ(err, (status_t)OK);
        setState(PAUSING);
        while (mState != PAUSED && mState != ERROR) {
            mAsyncCompletion.wait(mLock);
        }
        if (mState != ERROR)
            mPaused = true;
        return mState == ERROR ? UNKNOWN_ERROR : OK;
    } else {
        mPaused = true;
        return OK;
    }

}

status_t OMXCodec::resumeLocked(bool drainInputBuf) {
   CODEC_LOGV("resume mState=%d", mState);

   if (!strncmp(mComponentName, "OMX.qcom.", 9)) {
        while (isIntermediateState(mState)) {
            mAsyncCompletion.wait(mLock);
        }
        CHECK_EQ(mState, (status_t)PAUSED);
        status_t err = mOMX->sendCommand(mNode,
        OMX_CommandStateSet, OMX_StateExecuting);
        CHECK_EQ(err, (status_t)OK);
        setState(IDLE_TO_EXECUTING);
        mPaused = false;
        while (mState != EXECUTING && mState != ERROR) {
            mAsyncCompletion.wait(mLock);
        }
        if(drainInputBuf)
            drainInputBuffers();
        return mState == ERROR ? UNKNOWN_ERROR : OK;
    } else {   // SW Codec
        mPaused = false;
        if(drainInputBuf)
            drainInputBuffers();
        return OK;
    }
}

////////////////////////////////////////////////////////////////////////////////

#ifdef OMAP_ENHANCEMENT
void OMXCodec::restorePatchedDataPointer(BufferInfo *info) {
    CHECK(mIsEncoder && (mQuirks & kAvoidMemcopyInputRecordingFrames));
    CHECK(mOMXLivesLocally);

    OMX_BUFFERHEADERTYPE *header = (OMX_BUFFERHEADERTYPE *)info->mBuffer;
    header->pBuffer = (OMX_U8 *)info->mData;
}
#endif
status_t QueryCodecs(
        const sp<IOMX> &omx,
        const char *mime, bool queryDecoders, bool hwCodecOnly,
        Vector<CodecCapabilities> *results) {
    Vector<OMXCodec::CodecNameAndQuirks> matchingCodecs;
    results->clear();

    OMXCodec::findMatchingCodecs(mime,
            !queryDecoders /*createEncoder*/,
            NULL /*matchComponentName*/,
            hwCodecOnly ? OMXCodec::kHardwareCodecsOnly : 0 /*flags*/,
            &matchingCodecs);

    for (size_t c = 0; c < matchingCodecs.size(); c++) {
        const char *componentName = matchingCodecs.itemAt(c).mName.string();

        results->push();
        CodecCapabilities *caps = &results->editItemAt(results->size() - 1);

        status_t err =
            QueryCodec(omx, componentName, mime, !queryDecoders, caps);

        if (err != OK) {
            results->removeAt(results->size() - 1);
        }
    }

    return OK;
}

status_t QueryCodec(
        const sp<IOMX> &omx,
        const char *componentName, const char *mime,
        bool isEncoder,
        CodecCapabilities *caps) {
    if (strncmp(componentName, "OMX.", 4)) {
        // Not an OpenMax component but a software codec.
        caps->mFlags = 0;
        caps->mComponentName = componentName;
        return OK;
    }

    sp<OMXCodecObserver> observer = new OMXCodecObserver;
    IOMX::node_id node;
    status_t err = omx->allocateNode(componentName, observer, &node);

    if (err != OK) {
        return err;
    }

    OMXCodec::setComponentRole(omx, node, isEncoder, mime);

    caps->mFlags = 0;
    caps->mComponentName = componentName;

    OMX_VIDEO_PARAM_PROFILELEVELTYPE param;
    InitOMXParams(&param);

    param.nPortIndex = !isEncoder ? 0 : 1;

    for (param.nProfileIndex = 0;; ++param.nProfileIndex) {
        err = omx->getParameter(
                node, OMX_IndexParamVideoProfileLevelQuerySupported,
                &param, sizeof(param));

        if (err != OK) {
            break;
        }

        CodecProfileLevel profileLevel;
        profileLevel.mProfile = param.eProfile;
        profileLevel.mLevel = param.eLevel;

        caps->mProfileLevels.push(profileLevel);
    }

    // Color format query
    OMX_VIDEO_PARAM_PORTFORMATTYPE portFormat;
    InitOMXParams(&portFormat);
#ifdef OMAP_ENHANCEMENT
    portFormat.nPortIndex = !isEncoder ? 0 : 1;
#else
    portFormat.nPortIndex = !isEncoder ? 1 : 0;
#endif
    for (OMX_U32 index = 0;;index++) {
        portFormat.nIndex = index;
        err = omx->getParameter(
                node, OMX_IndexParamVideoPortFormat,
                &portFormat, sizeof(portFormat));
        if (err != OK) {
            break;
        }
        caps->mColorFormats.push(portFormat.eColorFormat);
    }

    if (!isEncoder && !strncmp(mime, "video/", 6)) {
        if (omx->storeMetaDataInBuffers(
                    node, 1 /* port index */, OMX_TRUE) == OK ||
            omx->prepareForAdaptivePlayback(
                    node, 1 /* port index */, OMX_TRUE,
                    1280 /* width */, 720 /* height */) == OK) {
            caps->mFlags |= CodecCapabilities::kFlagSupportsAdaptivePlayback;
        }
    }

    CHECK_EQ(omx->freeNode(node), (status_t)OK);

    return OK;
}

status_t QueryCodecs(
        const sp<IOMX> &omx,
        const char *mimeType, bool queryDecoders,
        Vector<CodecCapabilities> *results) {
    return QueryCodecs(omx, mimeType, queryDecoders, false /*hwCodecOnly*/, results);
}

// These are supposed be equivalent to the logic in
// "audio_channel_out_mask_from_count".
status_t getOMXChannelMapping(size_t numChannels, OMX_AUDIO_CHANNELTYPE map[]) {
    switch (numChannels) {
        case 1:
            map[0] = OMX_AUDIO_ChannelCF;
            break;
        case 2:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            break;
        case 3:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            break;
        case 4:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelLR;
            map[3] = OMX_AUDIO_ChannelRR;
            break;
        case 5:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            map[3] = OMX_AUDIO_ChannelLR;
            map[4] = OMX_AUDIO_ChannelRR;
            break;
        case 6:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            map[3] = OMX_AUDIO_ChannelLFE;
            map[4] = OMX_AUDIO_ChannelLR;
            map[5] = OMX_AUDIO_ChannelRR;
            break;
        case 7:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            map[3] = OMX_AUDIO_ChannelLFE;
            map[4] = OMX_AUDIO_ChannelLR;
            map[5] = OMX_AUDIO_ChannelRR;
            map[6] = OMX_AUDIO_ChannelCS;
            break;
        case 8:
            map[0] = OMX_AUDIO_ChannelLF;
            map[1] = OMX_AUDIO_ChannelRF;
            map[2] = OMX_AUDIO_ChannelCF;
            map[3] = OMX_AUDIO_ChannelLFE;
            map[4] = OMX_AUDIO_ChannelLR;
            map[5] = OMX_AUDIO_ChannelRR;
            map[6] = OMX_AUDIO_ChannelLS;
            map[7] = OMX_AUDIO_ChannelRS;
            break;
        default:
            return -EINVAL;
    }

    return OK;
}
bool OMXCodec::hasDisabledPorts() {
    if ((mPortStatus[kPortIndexOutput] == ENABLED) && (mPortStatus[kPortIndexInput] == ENABLED)) {
        return false;
    }
    return true;
}
status_t OMXCodec::releaseMediaBuffersOn(OMX_U32 portIndex) {
    if (mPortBuffers[portIndex].size() == 0) {
        return OK;
    }

    if (mState != ERROR) {
        CODEC_LOGE("assertion failure, needs to be investigated why %s "
              " buffers are still pending",
              portIndex == kPortIndexOutput ? "output" : "input");
    }

    Vector<BufferInfo> *buffers = &mPortBuffers[portIndex];

    for (size_t i = buffers->size(); i-- > 0;) {
        BufferInfo *info = &buffers->editItemAt(i);
        if (info->mMediaBuffer) {
            if (portIndex != (OMX_U32)kPortIndexOutput) {
                return UNKNOWN_ERROR;
            }
            info->mMediaBuffer->setObserver(NULL);

            // Make sure nobody but us owns this buffer at this point.
            if (info->mMediaBuffer->refcount() != 0) {
                return UNKNOWN_ERROR;
            }

            info->mMediaBuffer->release();
            info->mMediaBuffer = NULL;
        }
        buffers->removeAt(i);
    }
    return OK;
}

// Last resort to flush buffers and additionally cancel all native window buffers.
//lock _must_ be acquired in caller
status_t OMXCodec::flushBuffersOnError() {
    if (mState != ERROR) {
        return INVALID_OPERATION;
    }

    OMX_STATETYPE state = OMX_StateInvalid;
    status_t err = mOMX->getState(mNode, &state);
    if (err != OK) { //component is alive
        return err;
    }

    mPortStatus[kPortIndexOutput] = ENABLED;
    mPortStatus[kPortIndexInput] = ENABLED;

    setState(EXECUTING_TO_IDLE);

    flushPortAsync(kPortIndexOutput);
    flushPortAsync(kPortIndexInput);

    size_t kRetries = 15;

    bool outputBuffersPending =
        countBuffersWeOwn(mPortBuffers[kPortIndexOutput]) !=
        mPortBuffers[kPortIndexOutput].size();

    bool inputBuffersPending =
        countBuffersWeOwn(mPortBuffers[kPortIndexInput]) !=
        mPortBuffers[kPortIndexInput].size();

    setState(ERROR); //drop all except EBD/FBD
    while ((outputBuffersPending || inputBuffersPending) && --kRetries) {
        mLock.unlock();
        usleep(10000);
        mLock.lock();

        outputBuffersPending =
            countBuffersWeOwn(mPortBuffers[kPortIndexOutput]) !=
            mPortBuffers[kPortIndexOutput].size();

        inputBuffersPending =
            countBuffersWeOwn(mPortBuffers[kPortIndexInput]) !=
            mPortBuffers[kPortIndexInput].size();
    }

    if (inputBuffersPending || outputBuffersPending) {
        ALOGE("Timed out waiting for all input/output buffers to be returned, "
              "there might be a leak");
    }

    //additional work for native buffers
    if (mNativeWindow != NULL) {
        Vector<BufferInfo> *buffers = &mPortBuffers[kPortIndexOutput];
        for (size_t i = 0; i < buffers->size(); ++i) {
            BufferInfo *info = &buffers->editItemAt(i);
            if (info->mStatus == OWNED_BY_US) {
                cancelBufferToNativeWindow(info);
            }
        }
    }

    return OK;
}
}  // namespace android
