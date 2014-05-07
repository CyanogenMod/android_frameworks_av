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
#include "NdkMediaFormatPriv.h"

#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <gui/Surface.h>

#include <media/ICrypto.h>
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
    return -1000;
}


class CodecHandler: public AHandler {
public:
    CodecHandler(sp<android::MediaCodec>);
    virtual void onMessageReceived(const sp<AMessage> &msg);
};

CodecHandler::CodecHandler(sp<android::MediaCodec>) {

}

void CodecHandler::onMessageReceived(const sp<AMessage> &msg) {
    ALOGI("handler got message %d", msg->what());
}

struct AMediaCodec {
    sp<android::MediaCodec> mCodec;
    sp<ALooper> mLooper;
    sp<CodecHandler> mHandler;
};

extern "C" {

static AMediaCodec * createAMediaCodec(const char *name, bool name_is_type, bool encoder) {
    AMediaCodec *mData = new AMediaCodec();
    mData->mLooper = new ALooper;
    mData->mLooper->setName("NDK MediaCodec_looper");
    status_t ret = mData->mLooper->start(
            false,      // runOnCallingThread
            true,       // canCallJava XXX
            PRIORITY_FOREGROUND);
    ALOGV("looper start: %d", ret);
    if (name_is_type) {
        mData->mCodec = android::MediaCodec::CreateByType(mData->mLooper, name, encoder);
    } else {
        mData->mCodec = android::MediaCodec::CreateByComponentName(mData->mLooper, name);
    }
    mData->mHandler = new CodecHandler(mData->mCodec);
    mData->mLooper->registerHandler(mData->mHandler);
    return mData;
}


AMediaCodec* AMediaCodec_createByCodecName(const char *name) {
    return createAMediaCodec(name, false, false);
}

AMediaCodec* AMediaCodec_createByCodecType(const char *mime_type) {
    return createAMediaCodec(mime_type, true, false);
}

AMediaCodec* AMediaCodec_createEncoderByName(const char *name) {
    return createAMediaCodec(name, false, true);
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

int AMediaCodec_configure(AMediaCodec *mData, const AMediaFormat* format, ANativeWindow* window) {
    sp<AMessage> nativeFormat;
    AMediaFormat_getFormat(format, &nativeFormat);
    ALOGV("configure with format: %s", nativeFormat->debugString(0).c_str());
    sp<Surface> surface = NULL;
    if (window != NULL) {
        surface = (Surface*) window;
    }

    return translate_error(mData->mCodec->configure(nativeFormat, surface, NULL, 0));
}

int AMediaCodec_start(AMediaCodec *mData) {
    return translate_error(mData->mCodec->start());
}

int AMediaCodec_stop(AMediaCodec *mData) {
    return translate_error(mData->mCodec->stop());
}

int AMediaCodec_flush(AMediaCodec *mData) {
    return translate_error(mData->mCodec->flush());
}

ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *mData, int64_t timeoutUs) {
    size_t idx;
    status_t ret = mData->mCodec->dequeueInputBuffer(&idx, timeoutUs);
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

} // extern "C"

