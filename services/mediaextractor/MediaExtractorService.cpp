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

#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaExtractor.h>
#include "MediaExtractorService.h"

namespace android {

status_t MediaExtractorService::hello()
{
    ALOGI("@@@ MediaExtractorService::hello");
    return NO_ERROR;
}

sp<IMediaExtractor> MediaExtractorService::makeExtractor(
        const sp<IDataSource> &remoteSource, const char *mime) {
    ALOGI("@@@ MediaExtractorService::makeExtractor for %s", mime);

    sp<DataSource> localSource = DataSource::CreateFromIDataSource(remoteSource);

    sp<MediaExtractor> ret = MediaExtractor::CreateFromService(localSource, mime);

    ALOGI("extractor service created %p (%s)",
            ret.get(),
            ret == NULL ? "" : ret->name());

    return ret;
}


status_t MediaExtractorService::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
        uint32_t flags)
{
    return BnMediaExtractorService::onTransact(code, data, reply, flags);
}

}   // namespace android
