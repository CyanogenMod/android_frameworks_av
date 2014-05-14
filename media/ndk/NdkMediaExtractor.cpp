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


#include "NdkMediaError.h"
#include "NdkMediaExtractor.h"
#include "NdkMediaFormatPriv.h"


#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <media/hardware/CryptoAPI.h>
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
    return AMEDIAERROR_GENERIC;
}

struct AMediaExtractor {
    sp<NuMediaExtractor> mImpl;
    sp<ABuffer> mPsshBuf;

};

extern "C" {

EXPORT
AMediaExtractor* AMediaExtractor_new() {
    ALOGV("ctor");
    AMediaExtractor *mData = new AMediaExtractor();
    mData->mImpl = new NuMediaExtractor();
    return mData;
}

EXPORT
int AMediaExtractor_delete(AMediaExtractor *mData) {
    ALOGV("dtor");
    delete mData;
    return OK;
}

EXPORT
int AMediaExtractor_setDataSourceFd(AMediaExtractor *mData, int fd, off64_t offset, off64_t length) {
    ALOGV("setDataSource(%d, %lld, %lld)", fd, offset, length);
    mData->mImpl->setDataSource(fd, offset, length);
    return 0;
}

EXPORT
int AMediaExtractor_setDataSource(AMediaExtractor *mData, const char *location) {
    ALOGV("setDataSource(%s)", location);
    // TODO: add header support

    JNIEnv *env = AndroidRuntime::getJNIEnv();
    jobject service = NULL;
    if (env == NULL) {
        ALOGE("setDataSource(path) must be called from Java thread");
        env->ExceptionClear();
        return AMEDIAERROR_UNSUPPORTED;
    }

    jclass mediahttpclass = env->FindClass("android/media/MediaHTTPService");
    if (mediahttpclass == NULL) {
        ALOGE("can't find MediaHttpService");
        env->ExceptionClear();
        return AMEDIAERROR_UNSUPPORTED;
    }

    jmethodID mediaHttpCreateMethod = env->GetStaticMethodID(mediahttpclass,
            "createHttpServiceBinderIfNecessary", "(Ljava/lang/String;)Landroid/os/IBinder;");
    if (mediaHttpCreateMethod == NULL) {
        ALOGE("can't find method");
        env->ExceptionClear();
        return AMEDIAERROR_UNSUPPORTED;
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
    return OK;
}

EXPORT
int AMediaExtractor_getTrackCount(AMediaExtractor *mData) {
    return mData->mImpl->countTracks();
}

EXPORT
AMediaFormat* AMediaExtractor_getTrackFormat(AMediaExtractor *mData, size_t idx) {
    sp<AMessage> format;
    mData->mImpl->getTrackFormat(idx, &format);
    return AMediaFormat_fromMsg(&format);
}

EXPORT
int AMediaExtractor_selectTrack(AMediaExtractor *mData, size_t idx) {
    ALOGV("selectTrack(%z)", idx);
    return translate_error(mData->mImpl->selectTrack(idx));
}

EXPORT
int AMediaExtractor_unselectTrack(AMediaExtractor *mData, size_t idx) {
    ALOGV("unselectTrack(%z)", idx);
    return translate_error(mData->mImpl->unselectTrack(idx));
}

EXPORT
bool AMediaExtractor_advance(AMediaExtractor *mData) {
    //ALOGV("advance");
    return mData->mImpl->advance();
}

EXPORT
int AMediaExtractor_readSampleData(AMediaExtractor *mData, uint8_t *buffer, size_t capacity) {
    //ALOGV("readSampleData");
    sp<ABuffer> tmp = new ABuffer(buffer, capacity);
    if (mData->mImpl->readSampleData(tmp) == OK) {
        return tmp->size();
    }
    return -1;
}

EXPORT
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

EXPORT
int AMediaExtractor_getSampleTrackIndex(AMediaExtractor *mData) {
    size_t idx;
    if (mData->mImpl->getSampleTrackIndex(&idx) != OK) {
        return -1;
    }
    return idx;
}

EXPORT
int64_t AMediaExtractor_getSampletime(AMediaExtractor *mData) {
    int64_t time;
    if (mData->mImpl->getSampleTime(&time) != OK) {
        return -1;
    }
    return time;
}

EXPORT
PsshInfo* AMediaExtractor_getPsshInfo(AMediaExtractor *ex) {

    if (ex->mPsshBuf != NULL) {
        return (PsshInfo*) ex->mPsshBuf->data();
    }

    sp<AMessage> format;
    ex->mImpl->getFileFormat(&format);
    sp<ABuffer> buffer;
    if(!format->findBuffer("pssh", &buffer)) {
        return NULL;
    }

    // the format of the buffer is 1 or more of:
    //    {
    //        16 byte uuid
    //        4 byte data length N
    //        N bytes of data
    //    }

    // Determine the number of entries in the source data.
    // Since we got the data from stagefright, we trust it is valid and properly formatted.
    const uint8_t* data = buffer->data();
    size_t len = buffer->size();
    size_t numentries = 0;
    while (len > 0) {
        numentries++;

        // skip uuid
        data += 16;
        len -= 16;

        // get data length
        uint32_t datalen = *((uint32_t*)data);
        data += 4;
        len -= 4;

        // skip the data
        data += datalen;
        len -= datalen;
    }

    // there are <numentries> in the buffer, we need
    // (source buffer size) + 4 + (4 * numentries) bytes for the PsshInfo structure
    size_t newsize = buffer->size() + 4 + (4 * numentries);
    ex->mPsshBuf = new ABuffer(newsize);
    ex->mPsshBuf->setRange(0, newsize);

    // copy data
    const uint8_t* src = buffer->data();
    uint8_t* dst = ex->mPsshBuf->data();
    uint8_t* dstdata = dst + 4 + numentries * sizeof(PsshEntry);
    *((uint32_t*)dst) = numentries;
    dst += 4;
    for (size_t i = 0; i < numentries; i++) {
        // copy uuid
        memcpy(dst, src, 16);
        src += 16;
        dst += 16;

        // get/copy data length
        uint32_t datalen = *((uint32_t*)src);
        memcpy(dst, src, 4);
        src += 4;
        dst += 4;

        // the next entry in the destination is a pointer to the actual data, which we store
        // after the array of PsshEntry
        memcpy(dst, &dstdata, sizeof(dstdata));
        dst += 4;

        // copy the actual data
        memcpy(dstdata, src, datalen);
        dstdata += datalen;
        src += datalen;
    }

    return (PsshInfo*) ex->mPsshBuf->data();
}

EXPORT
AMediaCodecCryptoInfo *AMediaExtractor_getSampleCryptoInfo(AMediaExtractor *ex) {
    sp<MetaData> meta;
    if(ex->mImpl->getSampleMeta(&meta) != 0) {
        return NULL;
    }

    uint32_t type;
    const void *crypteddata;
    size_t cryptedsize;
    if (!meta->findData(kKeyEncryptedSizes, &type, &crypteddata, &cryptedsize)) {
        return NULL;
    }
    size_t numSubSamples = cryptedsize / sizeof(size_t);

    const void *cleardata;
    size_t clearsize;
    if (meta->findData(kKeyPlainSizes, &type, &cleardata, &clearsize)) {
        if (clearsize != cryptedsize) {
            // The two must be of the same length.
            return NULL;
        }
    }

    const void *key;
    size_t keysize;
    if (meta->findData(kKeyCryptoIV, &type, &key, &keysize)) {
        if (keysize != 16) {
            // IVs must be 16 bytes in length.
            return NULL;
        }
    }

    const void *iv;
    size_t ivsize;
    if (meta->findData(kKeyCryptoIV, &type, &iv, &ivsize)) {
        if (ivsize != 16) {
            // IVs must be 16 bytes in length.
            return NULL;
        }
    }

    int32_t mode;
    if (!meta->findInt32(kKeyCryptoMode, &mode)) {
        mode = CryptoPlugin::kMode_AES_CTR;
    }

    return AMediaCodecCryptoInfo_new(
            numSubSamples,
            (uint8_t*) key,
            (uint8_t*) iv,
            mode,
            (size_t*) cleardata,
            (size_t*) crypteddata);
}


} // extern "C"

