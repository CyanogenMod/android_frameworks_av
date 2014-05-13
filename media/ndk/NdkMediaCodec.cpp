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
#define LOG_TAG "NdkMediaCodec"

#include "NdkMediaCodec.h"
#include "NdkMediaError.h"
#include "NdkMediaCryptoPriv.h"
#include "NdkMediaFormatPriv.h"

#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <gui/Surface.h>

#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ABuffer.h>

#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaErrors.h>

using namespace android;


static int translate_error(status_t err) {
    if (err == OK) {
        return OK;
    } else if (err == -EAGAIN) {
        return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    }
    ALOGE("sf error code: %d", err);
    return AMEDIAERROR_GENERIC;
}

enum {
    kWhatActivityNotify,
    kWhatRequestActivityNotifications,
    kWhatStopActivityNotifications,
};


class CodecHandler: public AHandler {
private:
    AMediaCodec* mCodec;
public:
    CodecHandler(AMediaCodec *codec);
    virtual void onMessageReceived(const sp<AMessage> &msg);
};

struct AMediaCodec {
    sp<android::MediaCodec> mCodec;
    sp<ALooper> mLooper;
    sp<CodecHandler> mHandler;
    sp<AMessage> mActivityNotification;
    int32_t mGeneration;
    bool mRequestedActivityNotification;
    OnCodecEvent mCallback;
    void *mCallbackUserData;
};

CodecHandler::CodecHandler(AMediaCodec *codec) {
    mCodec = codec;
}

void CodecHandler::onMessageReceived(const sp<AMessage> &msg) {

    switch (msg->what()) {
        case kWhatRequestActivityNotifications:
        {
            if (mCodec->mRequestedActivityNotification) {
                break;
            }

            mCodec->mCodec->requestActivityNotification(mCodec->mActivityNotification);
            mCodec->mRequestedActivityNotification = true;
            break;
        }

        case kWhatActivityNotify:
        {
            {
                int32_t generation;
                msg->findInt32("generation", &generation);

                if (generation != mCodec->mGeneration) {
                    // stale
                    break;
                }

                mCodec->mRequestedActivityNotification = false;
            }

            if (mCodec->mCallback) {
                mCodec->mCallback(mCodec, mCodec->mCallbackUserData);
            }
            break;
        }

        case kWhatStopActivityNotifications:
        {
            uint32_t replyID;
            msg->senderAwaitsResponse(&replyID);

            mCodec->mGeneration++;
            mCodec->mRequestedActivityNotification = false;

            sp<AMessage> response = new AMessage;
            response->postReply(replyID);
            break;
        }

        default:
            ALOGE("shouldn't be here");
            break;
    }

}


static void requestActivityNotification(AMediaCodec *codec) {
    (new AMessage(kWhatRequestActivityNotifications, codec->mHandler->id()))->post();
}

extern "C" {

static AMediaCodec * createAMediaCodec(const char *name, bool name_is_type, bool encoder) {
    AMediaCodec *mData = new AMediaCodec();
    mData->mLooper = new ALooper;
    mData->mLooper->setName("NDK MediaCodec_looper");
    status_t ret = mData->mLooper->start(
            false,      // runOnCallingThread
            true,       // canCallJava XXX
            PRIORITY_FOREGROUND);
    if (name_is_type) {
        mData->mCodec = android::MediaCodec::CreateByType(mData->mLooper, name, encoder);
    } else {
        mData->mCodec = android::MediaCodec::CreateByComponentName(mData->mLooper, name);
    }
    mData->mHandler = new CodecHandler(mData);
    mData->mLooper->registerHandler(mData->mHandler);
    mData->mGeneration = 1;
    mData->mRequestedActivityNotification = false;
    mData->mCallback = NULL;

    return mData;
}


AMediaCodec* AMediaCodec_createCodecByName(const char *name) {
    return createAMediaCodec(name, false, false);
}

AMediaCodec* AMediaCodec_createDecoderByType(const char *mime_type) {
    return createAMediaCodec(mime_type, true, false);
}

AMediaCodec* AMediaCodec_createEncoderByType(const char *name) {
    return createAMediaCodec(name, true, true);
}

int AMediaCodec_delete(AMediaCodec *mData) {
    if (mData->mCodec != NULL) {
        mData->mCodec->release();
        mData->mCodec.clear();
    }

    if (mData->mLooper != NULL) {
        mData->mLooper->unregisterHandler(mData->mHandler->id());
        mData->mLooper->stop();
        mData->mLooper.clear();
    }
    delete mData;
    return OK;
}

int AMediaCodec_configure(
        AMediaCodec *mData,
        const AMediaFormat* format,
        ANativeWindow* window,
        AMediaCrypto *crypto,
        uint32_t flags) {
    sp<AMessage> nativeFormat;
    AMediaFormat_getFormat(format, &nativeFormat);
    ALOGV("configure with format: %s", nativeFormat->debugString(0).c_str());
    sp<Surface> surface = NULL;
    if (window != NULL) {
        surface = (Surface*) window;
    }

    return translate_error(mData->mCodec->configure(nativeFormat, surface,
            crypto ? crypto->mCrypto : NULL, flags));
}

int AMediaCodec_start(AMediaCodec *mData) {
    status_t ret =  mData->mCodec->start();
    if (ret != OK) {
        return translate_error(ret);
    }
    mData->mActivityNotification = new AMessage(kWhatActivityNotify, mData->mHandler->id());
    mData->mActivityNotification->setInt32("generation", mData->mGeneration);
    requestActivityNotification(mData);
    return OK;
}

int AMediaCodec_stop(AMediaCodec *mData) {
    int ret = translate_error(mData->mCodec->stop());

    sp<AMessage> msg = new AMessage(kWhatStopActivityNotifications, mData->mHandler->id());
    sp<AMessage> response;
    msg->postAndAwaitResponse(&response);
    mData->mActivityNotification.clear();

    return ret;
}

int AMediaCodec_flush(AMediaCodec *mData) {
    return translate_error(mData->mCodec->flush());
}

ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *mData, int64_t timeoutUs) {
    size_t idx;
    status_t ret = mData->mCodec->dequeueInputBuffer(&idx, timeoutUs);
    requestActivityNotification(mData);
    if (ret == OK) {
        return idx;
    }
    return translate_error(ret);
}

uint8_t* AMediaCodec_getInputBuffer(AMediaCodec *mData, size_t idx, size_t *out_size) {
    android::Vector<android::sp<android::ABuffer> > abufs;
    if (mData->mCodec->getInputBuffers(&abufs) == 0) {
        size_t n = abufs.size();
        if (idx >= n) {
            ALOGE("buffer index %d out of range", idx);
            return NULL;
        }
        if (out_size != NULL) {
            *out_size = abufs[idx]->capacity();
        }
        return abufs[idx]->data();
    }
    ALOGE("couldn't get input buffers");
    return NULL;
}

uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec *mData, size_t idx, size_t *out_size) {
    android::Vector<android::sp<android::ABuffer> > abufs;
    if (mData->mCodec->getOutputBuffers(&abufs) == 0) {
        size_t n = abufs.size();
        if (idx >= n) {
            ALOGE("buffer index %d out of range", idx);
            return NULL;
        }
        if (out_size != NULL) {
            *out_size = abufs[idx]->capacity();
        }
        return abufs[idx]->data();
    }
    ALOGE("couldn't get output buffers");
    return NULL;
}

int AMediaCodec_queueInputBuffer(AMediaCodec *mData,
        size_t idx, off_t offset, size_t size, uint64_t time, uint32_t flags) {

    AString errorMsg;
    status_t ret = mData->mCodec->queueInputBuffer(idx, offset, size, time, flags, &errorMsg);
    return translate_error(ret);
}

ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *mData,
        AMediaCodecBufferInfo *info, int64_t timeoutUs) {
    size_t idx;
    size_t offset;
    size_t size;
    uint32_t flags;
    int64_t presentationTimeUs;
    status_t ret = mData->mCodec->dequeueOutputBuffer(&idx, &offset, &size, &presentationTimeUs,
            &flags, timeoutUs);
    requestActivityNotification(mData);
    switch (ret) {
        case OK:
            info->offset = offset;
            info->size = size;
            info->flags = flags;
            info->presentationTimeUs = presentationTimeUs;
            return idx;
        case -EAGAIN:
            return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        case android::INFO_FORMAT_CHANGED:
            return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;
        case INFO_OUTPUT_BUFFERS_CHANGED:
            return AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED;
        default:
            break;
    }
    return translate_error(ret);
}

AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec *mData) {
    sp<AMessage> format;
    mData->mCodec->getOutputFormat(&format);
    return AMediaFormat_fromMsg(&format);
}

int AMediaCodec_releaseOutputBuffer(AMediaCodec *mData, size_t idx, bool render) {
    if (render) {
        return translate_error(mData->mCodec->renderOutputBufferAndRelease(idx));
    } else {
        return translate_error(mData->mCodec->releaseOutputBuffer(idx));
    }
}

int AMediaCodec_setNotificationCallback(AMediaCodec *mData, OnCodecEvent callback, void *userdata) {
    mData->mCallback = callback;
    mData->mCallbackUserData = userdata;
    return OK;
}

typedef struct AMediaCodecCryptoInfo {
        int numsubsamples;
        uint8_t key[16];
        uint8_t iv[16];
        uint32_t mode;
        size_t *clearbytes;
        size_t *encryptedbytes;
} AMediaCodecCryptoInfo;

int AMediaCodec_queueSecureInputBuffer(
        AMediaCodec* codec,
        size_t idx,
        off_t offset,
        AMediaCodecCryptoInfo* crypto,
        uint64_t time,
        uint32_t flags) {

    CryptoPlugin::SubSample *subSamples = new CryptoPlugin::SubSample[crypto->numsubsamples];
    for (int i = 0; i < crypto->numsubsamples; i++) {
        subSamples[i].mNumBytesOfClearData = crypto->clearbytes[i];
        subSamples[i].mNumBytesOfEncryptedData = crypto->encryptedbytes[i];
    }

    AString errormsg;
    status_t err  = codec->mCodec->queueSecureInputBuffer(idx,
            offset,
            subSamples,
            crypto->numsubsamples,
            crypto->key,
            crypto->iv,
            (CryptoPlugin::Mode) crypto->mode,
            time,
            flags,
            &errormsg);
    if (err != 0) {
        ALOGE("queSecureInputBuffer: %s", errormsg.c_str());
    }
    delete [] subSamples;
    return translate_error(err);
}



AMediaCodecCryptoInfo *AMediaCodecCryptoInfo_new(
        int numsubsamples,
        uint8_t key[16],
        uint8_t iv[16],
        uint32_t mode,
        size_t *clearbytes,
        size_t *encryptedbytes) {

    // size needed to store all the crypto data
    size_t cryptosize = sizeof(AMediaCodecCryptoInfo) + sizeof(size_t) * numsubsamples * 2;
    AMediaCodecCryptoInfo *ret = (AMediaCodecCryptoInfo*) malloc(cryptosize);
    if (!ret) {
        ALOGE("couldn't allocate %d bytes", cryptosize);
        return NULL;
    }
    ret->numsubsamples = numsubsamples;
    memcpy(ret->key, key, 16);
    memcpy(ret->iv, iv, 16);
    ret->mode = mode;

    // clearbytes and encryptedbytes point at the actual data, which follows
    ret->clearbytes = (size_t*) (ret + 1); // point immediately after the struct
    ret->encryptedbytes = ret->clearbytes + numsubsamples; // point after the clear sizes

    memcpy(ret->clearbytes, clearbytes, numsubsamples * sizeof(size_t));
    memcpy(ret->encryptedbytes, encryptedbytes, numsubsamples * sizeof(size_t));

    return ret;
}


int AMediaCodecCryptoInfo_delete(AMediaCodecCryptoInfo* info) {
    free(info);
    return OK;
}

size_t AMediaCodecCryptoInfo_getNumSubSamples(AMediaCodecCryptoInfo* ci) {
    return ci->numsubsamples;
}

int AMediaCodecCryptoInfo_getKey(AMediaCodecCryptoInfo* ci, uint8_t *dst) {
    if (!dst || !ci) {
        return AMEDIAERROR_UNSUPPORTED;
    }
    memcpy(dst, ci->key, 16);
    return OK;
}

int AMediaCodecCryptoInfo_getIV(AMediaCodecCryptoInfo* ci, uint8_t *dst) {
    if (!dst || !ci) {
        return AMEDIAERROR_UNSUPPORTED;
    }
    memcpy(dst, ci->iv, 16);
    return OK;
}

uint32_t AMediaCodecCryptoInfo_getMode(AMediaCodecCryptoInfo* ci) {
    if (!ci) {
        return AMEDIAERROR_UNSUPPORTED;
    }
    return ci->mode;
}

int AMediaCodecCryptoInfo_getClearBytes(AMediaCodecCryptoInfo* ci, size_t *dst) {
    if (!dst || !ci) {
        return AMEDIAERROR_UNSUPPORTED;
    }
    memcpy(dst, ci->clearbytes, sizeof(size_t) * ci->numsubsamples);
    return OK;
}

int AMediaCodecCryptoInfo_getEncryptedBytes(AMediaCodecCryptoInfo* ci, size_t *dst) {
    if (!dst || !ci) {
        return AMEDIAERROR_UNSUPPORTED;
    }
    memcpy(dst, ci->encryptedbytes, sizeof(size_t) * ci->numsubsamples);
    return OK;
}

} // extern "C"

