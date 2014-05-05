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

#define LOG_NDEBUG 0
#define LOG_TAG "NdkMediaExtractor"


#include "NdkMediaExtractor.h"
#include "NdkMediaFormatPriv.h"


#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/NuMediaExtractor.h>
#include <media/IMediaHTTPService.h>
#include <android_runtime/AndroidRuntime.h>
#include <android_util_Binder.h>

#include <jni.h>

using namespace android;

static int translate_error(status_t err) {
    if (err == OK) {
        return OK;
    }
    ALOGE("sf error code: %d", err);
    return -1000;
}

struct AMediaExtractor {
    sp<NuMediaExtractor> mImpl;

};

extern "C" {

AMediaExtractor* AMediaExtractor_new() {
    ALOGV("ctor");
    AMediaExtractor *mData = new AMediaExtractor();
    mData->mImpl = new NuMediaExtractor();
    return mData;
}

int AMediaExtractor_delete(AMediaExtractor *mData) {
    ALOGV("dtor");
    delete mData;
    return OK;
}

int AMediaExtractor_setDataSourceFd(AMediaExtractor *mData, int fd, off64_t offset, off64_t length) {
    ALOGV("setDataSource(%d, %lld, %lld)", fd, offset, length);
    mData->mImpl->setDataSource(fd, offset, length);
    return 0;
}

int AMediaExtractor_setDataSource(AMediaExtractor *mData, const char *location) {
    ALOGV("setDataSource(%s)", location);
    // TODO: add header support

    JNIEnv *env = AndroidRuntime::getJNIEnv();
    jobject service = NULL;
    if (env == NULL) {
        ALOGE("setDataSource(path) must be called from Java thread");
        env->ExceptionClear();
        return -1;
    }

    jclass mediahttpclass = env->FindClass("android/media/MediaHTTPService");
    if (mediahttpclass == NULL) {
        ALOGE("can't find MediaHttpService");
        env->ExceptionClear();
        return -1;
    }

    jmethodID mediaHttpCreateMethod = env->GetStaticMethodID(mediahttpclass,
            "createHttpServiceBinderIfNecessary", "(Ljava/lang/String;)Landroid/os/IBinder;");
    if (mediaHttpCreateMethod == NULL) {
        ALOGE("can't find method");
        env->ExceptionClear();
        return -1;
    }

    jstring jloc = env->NewStringUTF(location);

    service = env->CallStaticObjectMethod(mediahttpclass, mediaHttpCreateMethod, jloc);
    env->DeleteLocalRef(jloc);

    sp<IMediaHTTPService> httpService;
    if (service != NULL) {
        sp<IBinder> binder = ibinderForJavaObject(env, service);
        httpService = interface_cast<IMediaHTTPService>(binder);
    }

    mData->mImpl->setDataSource(httpService, location, NULL);
    env->ExceptionClear();
    return 0;
}

int AMediaExtractor_getTrackCount(AMediaExtractor *mData) {
    return mData->mImpl->countTracks();
}

AMediaFormat* AMediaExtractor_getTrackFormat(AMediaExtractor *mData, size_t idx) {
    sp<AMessage> format;
    mData->mImpl->getTrackFormat(idx, &format);
    return AMediaFormat_fromMsg(&format);
}

int AMediaExtractor_selectTrack(AMediaExtractor *mData, size_t idx) {
    ALOGV("selectTrack(%z)", idx);
    return translate_error(mData->mImpl->selectTrack(idx));
}

int AMediaExtractor_unselectTrack(AMediaExtractor *mData, size_t idx) {
    ALOGV("unselectTrack(%z)", idx);
    return translate_error(mData->mImpl->unselectTrack(idx));
}

bool AMediaExtractor_advance(AMediaExtractor *mData) {
    //ALOGV("advance");
    return mData->mImpl->advance();
}

int AMediaExtractor_readSampleData(AMediaExtractor *mData, uint8_t *buffer, size_t capacity) {
    //ALOGV("readSampleData");
    sp<ABuffer> tmp = new ABuffer(buffer, capacity);
    if (mData->mImpl->readSampleData(tmp) == OK) {
        return tmp->size();
    }
    return -1;
}

int AMediaExtractor_getSampleFlags(AMediaExtractor *mData) {
    int sampleFlags = 0;
    sp<MetaData> meta;
    status_t err = mData->mImpl->getSampleMeta(&meta);
    if (err != OK) {
        return -1;
    }
    int32_t val;
    if (meta->findInt32(kKeyIsSyncFrame, &val) && val != 0) {
        sampleFlags |= NuMediaExtractor::SAMPLE_FLAG_SYNC;
    }

    uint32_t type;
    const void *data;
    size_t size;
    if (meta->findData(kKeyEncryptedSizes, &type, &data, &size)) {
        sampleFlags |= NuMediaExtractor::SAMPLE_FLAG_ENCRYPTED;
    }
    return sampleFlags;
}

int AMediaExtractor_getSampleTrackIndex(AMediaExtractor *mData) {
    size_t idx;
    if (mData->mImpl->getSampleTrackIndex(&idx) != OK) {
        return -1;
    }
    return idx;
}

int64_t AMediaExtractor_getSampletime(AMediaExtractor *mData) {
    int64_t time;
    if (mData->mImpl->getSampleTime(&time) != OK) {
        return -1;
    }
    return time;
}


} // extern "C"

