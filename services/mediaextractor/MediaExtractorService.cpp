/*
 * Copyright (C) 2013 The Android Open Source Project
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

#define LOG_TAG "MediaExtractorService"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <utils/Vector.h>

#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaExtractor.h>
#include "MediaExtractorService.h"

namespace android {

typedef struct {
    String8 mime;
    String8 name;
    pid_t owner;
    wp<MediaExtractor> extractor;
    String8 toString() {
        String8 str = name;
        str.append(" for mime ");
        str.append(mime);
        str.append(String8::format(", pid %d: ", owner));
        if (extractor.promote() == NULL) {
            str.append("deleted");
        } else {
            str.append("active");
        }
        return str;
    }
} ExtractorInstance;

static Vector<ExtractorInstance> extractors;

sp<IMediaExtractor> MediaExtractorService::makeExtractor(
        const sp<IDataSource> &remoteSource, const char *mime) {
    ALOGV("@@@ MediaExtractorService::makeExtractor for %s", mime);

    sp<DataSource> localSource = DataSource::CreateFromIDataSource(remoteSource);

    sp<MediaExtractor> ret = MediaExtractor::CreateFromService(localSource, mime);

    ALOGV("extractor service created %p (%s)",
            ret.get(),
            ret == NULL ? "" : ret->name());

    if (ret != NULL) {
        ExtractorInstance ex;
        ex.mime = mime == NULL ? "NULL" : mime;
        ex.name = ret->name();
        ex.owner = IPCThreadState::self()->getCallingPid();
        ex.extractor = ret;

        if (extractors.size() > 10) {
            extractors.resize(10);
        }
        extractors.push_front(ex);
    }

    return ret;
}

status_t MediaExtractorService::dump(int fd, const Vector<String16>& args) {
    String8 out;
    out.append("Recent extractors, most recent first:\n");
    for (size_t i = 0; i < extractors.size(); i++) {
        ExtractorInstance ex = extractors.itemAt(i);
        out.append("  ");
        out.append(ex.toString());
        out.append("\n");
    }
    write(fd, out.string(), out.size());
    return OK;
}


status_t MediaExtractorService::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
        uint32_t flags)
{
    return BnMediaExtractorService::onTransact(code, data, reply, flags);
}

}   // namespace android
