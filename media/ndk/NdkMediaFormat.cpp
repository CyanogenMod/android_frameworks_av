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
#define LOG_TAG "NdkMediaFormat"


#include "NdkMediaFormat.h"

#include <utils/Log.h>
#include <utils/StrongPointer.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MetaData.h>
#include <android_runtime/AndroidRuntime.h>
#include <android_util_Binder.h>

#include <jni.h>

using namespace android;

struct AMediaFormat {
    sp<AMessage> mFormat;
};

extern "C" {

// private functions for conversion to/from AMessage
AMediaFormat* AMediaFormat_fromMsg(void* data) {
    ALOGV("private ctor");
    AMediaFormat* mData = new AMediaFormat();
    mData->mFormat = *((sp<AMessage>*)data);
    return mData;
}

void AMediaFormat_getFormat(AMediaFormat* mData, void* dest) {
    *((sp<AMessage>*)dest) = mData->mFormat;
}


/*
 * public function follow
 */
AMediaFormat *AMediaFormat_new() {
    ALOGV("ctor");
    sp<AMessage> msg = new AMessage();
    return AMediaFormat_fromMsg(&msg);
}

int AMediaFormat_delete(AMediaFormat *mData) {
    ALOGV("dtor");
    delete mData;
    return OK;
}


const char* AMediaFormat_toString(AMediaFormat *mData) {
    sp<AMessage> f = mData->mFormat;
    String8 ret;
    int num = f->countEntries();
    for (int i = 0; i < num; i++) {
        if (i != 0) {
            ret.append(", ");
        }
        AMessage::Type t;
        const char *name = f->getEntryNameAt(i, &t);
        ret.append(name);
        ret.append(": ");
        switch (t) {
            case AMessage::kTypeInt32:
            {
                int32_t val;
                f->findInt32(name, &val);
                ret.appendFormat("int32(%d)", val);
                break;
            }
            case AMessage::kTypeInt64:
            {
                int64_t val;
                f->findInt64(name, &val);
                ret.appendFormat("int64(%lld)", val);
                break;
            }
            case AMessage::kTypeSize:
            {
                size_t val;
                f->findSize(name, &val);
                ret.appendFormat("size_t(%d)", val);
                break;
            }
            case AMessage::kTypeFloat:
            {
                float val;
                f->findFloat(name, &val);
                ret.appendFormat("float(%f)", val);
                break;
            }
            case AMessage::kTypeDouble:
            {
                double val;
                f->findDouble(name, &val);
                ret.appendFormat("double(%f)", val);
                break;
            }
            case AMessage::kTypeString:
            {
                AString val;
                f->findString(name, &val);
                ret.appendFormat("string(%s)", val.c_str());
                break;
            }
            case AMessage::kTypeBuffer:
            {
                ret.appendFormat("data");
                break;
            }
            default:
            {
                ret.appendFormat("unknown(%d)", t);
                break;
            }
        }
    }
    ret.append("}");
    return strdup(ret.string());
}

bool AMediaFormat_getInt32(AMediaFormat* mData, const char *name, int32_t *out) {
    return mData->mFormat->findInt32(name, out);
}

bool AMediaFormat_getInt64(AMediaFormat* mData, const char *name, int64_t *out) {
    return mData->mFormat->findInt64(name, out);
}

bool AMediaFormat_getFloat(AMediaFormat* mData, const char *name, float *out) {
    return mData->mFormat->findFloat(name, out);
}

bool AMediaFormat_getDouble(AMediaFormat* mData, const char *name, double *out) {
    return mData->mFormat->findDouble(name, out);
}

bool AMediaFormat_getSize(AMediaFormat* mData, const char *name, size_t *out) {
    return mData->mFormat->findSize(name, out);
}

bool AMediaFormat_getString(AMediaFormat* mData, const char *name, const char **out) {
    AString tmp;
    if (mData->mFormat->findString(name, &tmp)) {
        *out = strdup(tmp.c_str());
        return true;
    }
    return false;
}

const char* AMEDIAFORMAT_KEY_AAC_PROFILE = "aac-profile";
const char* AMEDIAFORMAT_KEY_BIT_RATE = "bitrate";
const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
const char* AMEDIAFORMAT_KEY_CHANNEL_MASK = "channel-mask";
const char* AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";
const char* AMEDIAFORMAT_KEY_DURATION = "durationUs";
const char* AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL = "flac-compression-level";
const char* AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate";
const char* AMEDIAFORMAT_KEY_HEIGHT = "height";
const char* AMEDIAFORMAT_KEY_IS_ADTS = "is-adts";
const char* AMEDIAFORMAT_KEY_IS_AUTOSELECT = "is-autoselect";
const char* AMEDIAFORMAT_KEY_IS_DEFAULT = "is-default";
const char* AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE = "is-forced-subtitle";
const char* AMEDIAFORMAT_KEY_I_FRAME_INTERVAL = "i-frame-interval";
const char* AMEDIAFORMAT_KEY_LANGUAGE = "language";
const char* AMEDIAFORMAT_KEY_MAX_HEIGHT = "max-height";
const char* AMEDIAFORMAT_KEY_MAX_INPUT_SIZE = "max-input-size";
const char* AMEDIAFORMAT_KEY_MAX_WIDTH = "max-width";
const char* AMEDIAFORMAT_KEY_MIME = "mime";
const char* AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP = "push-blank-buffers-on-shutdown";
const char* AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER = "repeat-previous-frame-after";
const char* AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
const char* AMEDIAFORMAT_KEY_WIDTH = "width";
const char* AMEDIAFORMAT_KEY_STRIDE = "stride";


} // extern "C"


