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
#define LOG_TAG "DataSource"

#include "include/AMRExtractor.h"

#include "include/AACExtractor.h"
#include "include/DRMExtractor.h"
#include "include/FLACExtractor.h"
#include "include/HTTPBase.h"
#include "include/MP3Extractor.h"
#include "include/MPEG2PSExtractor.h"
#include "include/MPEG2TSExtractor.h"
#include "include/MPEG4Extractor.h"
#include "include/NuCachedSource2.h"
#include "include/OggExtractor.h"
#include "include/WAVExtractor.h"
#include "include/WVMExtractor.h"
#ifdef QCOM_HARDWARE
#include "include/ExtendedExtractor.h"
#endif
#include "matroska/MatroskaExtractor.h"

#include <media/IMediaHTTPConnection.h>
#include <media/IMediaHTTPService.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/DataURISource.h>
#include <media/stagefright/FileSource.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaHTTP.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <cutils/log.h>

#include <dlfcn.h>

namespace android {

static void *loadExtractorPlugin() {
    void *ret = NULL;
    char lib[PROPERTY_VALUE_MAX];
    if (property_get("media.sf.extractor-plugin", lib, NULL)) {
        if (lib != NULL) {
            if (void *extractorLib = ::dlopen(lib, RTLD_LAZY)) {
                ret = ::dlsym(extractorLib, "getExtractorPlugin");
                ALOGW_IF(!ret, "Failed to find symbol, dlerror: %s", ::dlerror());
            } else {
                ALOGV("Failed to load %s, dlerror: %s", lib, ::dlerror());
            }
        }
    }
    return ret;
}

bool DataSource::getUInt16(off64_t offset, uint16_t *x) {
    *x = 0;

    uint8_t byte[2];
    if (readAt(offset, byte, 2) != 2) {
        return false;
    }

    *x = (byte[0] << 8) | byte[1];

    return true;
}

bool DataSource::getUInt24(off64_t offset, uint32_t *x) {
    *x = 0;

    uint8_t byte[3];
    if (readAt(offset, byte, 3) != 3) {
        return false;
    }

    *x = (byte[0] << 16) | (byte[1] << 8) | byte[2];

    return true;
}

bool DataSource::getUInt32(off64_t offset, uint32_t *x) {
    *x = 0;

    uint32_t tmp;
    if (readAt(offset, &tmp, 4) != 4) {
        return false;
    }

    *x = ntohl(tmp);

    return true;
}

bool DataSource::getUInt64(off64_t offset, uint64_t *x) {
    *x = 0;

    uint64_t tmp;
    if (readAt(offset, &tmp, 8) != 8) {
        return false;
    }

    *x = ntoh64(tmp);

    return true;
}

status_t DataSource::getSize(off64_t *size) {
    *size = 0;

    return ERROR_UNSUPPORTED;
}

////////////////////////////////////////////////////////////////////////////////

bool DataSource::sniff(
        String8 *mimeType, float *confidence, sp<AMessage> *meta) {

    return  mSniffer->sniff(this, mimeType, confidence, meta);
}

// static
void DataSource::RegisterSniffer_l(SnifferFunc func) {
    return;
}

// static
void DataSource::RegisterDefaultSniffers() {
    return;
}

////////////////////////////////////////////////////////////////////////////////

Sniffer::Sniffer() {
    registerDefaultSniffers();
}

bool Sniffer::sniff(
        DataSource *source, String8 *mimeType, float *confidence, sp<AMessage> *meta) {

    bool forceExtraSniffers = false;

    if (*confidence == 3.14f) {
       // Magic value, as set by MediaExtractor when a video container looks incomplete
       forceExtraSniffers = true;
    }

    *mimeType = "";
    *confidence = 0.0f;
    meta->clear();

    Mutex::Autolock autoLock(mSnifferMutex);
    for (List<SnifferFunc>::iterator it = mSniffers.begin();
         it != mSniffers.end(); ++it) {
        String8 newMimeType;
        float newConfidence;
        sp<AMessage> newMeta;
        if ((*it)(source, &newMimeType, &newConfidence, &newMeta)) {
            if (newConfidence > *confidence) {
                *mimeType = newMimeType;
                *confidence = newConfidence;
                *meta = newMeta;
            }
        }
    }

    /* Only do the deeper sniffers if the results are null or in doubt */
    if (mimeType->length() == 0 || *confidence < 0.21f || forceExtraSniffers) {
        for (List<SnifferFunc>::iterator it = mExtraSniffers.begin();
                it != mExtraSniffers.end(); ++it) {
            String8 newMimeType;
            float newConfidence;
            sp<AMessage> newMeta;
            if ((*it)(source, &newMimeType, &newConfidence, &newMeta)) {
                if (newConfidence > *confidence) {
                    *mimeType = newMimeType;
                    *confidence = newConfidence;
                    *meta = newMeta;
                }
            }
        }
    }

    return *confidence > 0.0;
}

void Sniffer::registerSniffer_l(SnifferFunc func) {

    for (List<SnifferFunc>::iterator it = mSniffers.begin();
         it != mSniffers.end(); ++it) {
        if (*it == func) {
            return;
        }
    }

    mSniffers.push_back(func);
}

void Sniffer::registerSnifferPlugin() {
    static void (*getExtractorPlugin)(MediaExtractor::Plugin *) =
            (void (*)(MediaExtractor::Plugin *))loadExtractorPlugin();

    MediaExtractor::Plugin *plugin = MediaExtractor::getPlugin();
    if (!plugin->sniff && getExtractorPlugin) {
        getExtractorPlugin(plugin);
    }
    if (plugin->sniff) {
        for (List<SnifferFunc>::iterator it = mExtraSniffers.begin();
             it != mExtraSniffers.end(); ++it) {
            if (*it == plugin->sniff) {
                return;
            }
        }

        mExtraSniffers.push_back(plugin->sniff);
    }
}

void Sniffer::registerDefaultSniffers() {
    Mutex::Autolock autoLock(mSnifferMutex);

    registerSniffer_l(SniffMPEG4);
    registerSniffer_l(SniffMatroska);
    registerSniffer_l(SniffOgg);
    registerSniffer_l(SniffWAV);
    registerSniffer_l(SniffFLAC);
    registerSniffer_l(SniffAMR);
    registerSniffer_l(SniffMPEG2TS);
    registerSniffer_l(SniffMP3);
    registerSniffer_l(SniffAAC);
    registerSniffer_l(SniffMPEG2PS);
    registerSniffer_l(SniffWVM);
#ifdef QCOM_HARDWARE
    registerSniffer_l(ExtendedExtractor::Sniff);
#endif
    registerSnifferPlugin();

    char value[PROPERTY_VALUE_MAX];
    if (property_get("drm.service.enabled", value, NULL)
            && (!strcmp(value, "1") || !strcasecmp(value, "true"))) {
        registerSniffer_l(SniffDRM);
    }
}

// static
sp<DataSource> DataSource::CreateFromURI(
        const sp<IMediaHTTPService> &httpService,
        const char *uri,
        const KeyedVector<String8, String8> *headers,
        String8 *contentType,
        HTTPBase *httpSource) {
    if (contentType != NULL) {
        *contentType = "";
    }

    bool isWidevine = !strncasecmp("widevine://", uri, 11);

    sp<DataSource> source;
    if (!strncasecmp("file://", uri, 7)) {
        source = new FileSource(uri + 7);
    } else if (!strncasecmp("http://", uri, 7)
            || !strncasecmp("https://", uri, 8)
            || isWidevine) {
        if (httpService == NULL) {
            ALOGE("Invalid http service!");
            return NULL;
        }

        if (httpSource == NULL) {
            sp<IMediaHTTPConnection> conn = httpService->makeHTTPConnection();
            if (conn == NULL) {
                ALOGE("Failed to make http connection from http service!");
                return NULL;
            }
            httpSource = new MediaHTTP(conn);
        }

        String8 tmp;
        if (isWidevine) {
            tmp = String8("http://");
            tmp.append(uri + 11);

            uri = tmp.string();
        }

        String8 cacheConfig;
        bool disconnectAtHighwatermark;
        KeyedVector<String8, String8> nonCacheSpecificHeaders;
        if (headers != NULL) {
            nonCacheSpecificHeaders = *headers;
            NuCachedSource2::RemoveCacheSpecificHeaders(
                    &nonCacheSpecificHeaders,
                    &cacheConfig,
                    &disconnectAtHighwatermark);
        }

        if (httpSource->connect(uri, &nonCacheSpecificHeaders) != OK) {
            ALOGE("Failed to connect http source!");
            return NULL;
        }

        if (!isWidevine) {
            if (contentType != NULL) {
                *contentType = httpSource->getMIMEType();
            }

            source = new NuCachedSource2(
                    httpSource,
                    cacheConfig.isEmpty() ? NULL : cacheConfig.string(),
                    disconnectAtHighwatermark);
        } else {
            // We do not want that prefetching, caching, datasource wrapper
            // in the widevine:// case.
            source = httpSource;
        }
    } else if (!strncasecmp("data:", uri, 5)) {
        source = DataURISource::Create(uri);
    } else {
        // Assume it's a filename.
        source = new FileSource(uri);
    }

    if (source == NULL || source->initCheck() != OK) {
        return NULL;
    }

    return source;
}

sp<DataSource> DataSource::CreateMediaHTTP(const sp<IMediaHTTPService> &httpService) {
    if (httpService == NULL) {
        return NULL;
    }

    sp<IMediaHTTPConnection> conn = httpService->makeHTTPConnection();
    if (conn == NULL) {
        return NULL;
    } else {
        return new MediaHTTP(conn);
    }
}

String8 DataSource::getMIMEType() const {
    return String8("application/octet-stream");
}

}  // namespace android
