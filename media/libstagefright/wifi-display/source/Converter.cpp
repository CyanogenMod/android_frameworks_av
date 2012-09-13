/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "Converter"
#include <utils/Log.h>

#include "Converter.h"

#include <cutils/properties.h>
#include <gui/SurfaceTextureClient.h>
#include <media/ICrypto.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>

namespace android {

Converter::Converter(
        const sp<AMessage> &notify,
        const sp<ALooper> &codecLooper,
        const sp<AMessage> &format)
    : mInitCheck(NO_INIT),
      mNotify(notify),
      mCodecLooper(codecLooper),
      mInputFormat(format),
      mDoMoreWorkPending(false) {
    mInitCheck = initEncoder();
}

Converter::~Converter() {
    if (mEncoder != NULL) {
        mEncoder->release();
        mEncoder.clear();
    }

    AString mime;
    CHECK(mInputFormat->findString("mime", &mime));
    ALOGI("encoder (%s) shut down.", mime.c_str());
}

status_t Converter::initCheck() const {
    return mInitCheck;
}

size_t Converter::getInputBufferCount() const {
    return mEncoderInputBuffers.size();
}

sp<AMessage> Converter::getOutputFormat() const {
    return mOutputFormat;
}

static int32_t getBitrate(const char *propName, int32_t defaultValue) {
    char val[PROPERTY_VALUE_MAX];
    if (property_get(propName, val, NULL)) {
        char *end;
        unsigned long x = strtoul(val, &end, 10);

        if (*end == '\0' && end > val && x > 0) {
            return x;
        }
    }

    return defaultValue;
}

status_t Converter::initEncoder() {
    AString inputMIME;
    CHECK(mInputFormat->findString("mime", &inputMIME));

    AString outputMIME;
    bool isAudio = false;
    if (!strcasecmp(inputMIME.c_str(), MEDIA_MIMETYPE_AUDIO_RAW)) {
        outputMIME = MEDIA_MIMETYPE_AUDIO_AAC;
        isAudio = true;
    } else if (!strcasecmp(inputMIME.c_str(), MEDIA_MIMETYPE_VIDEO_RAW)) {
        outputMIME = MEDIA_MIMETYPE_VIDEO_AVC;
    } else {
        TRESPASS();
    }

    mEncoder = MediaCodec::CreateByType(
            mCodecLooper, outputMIME.c_str(), true /* encoder */);

    if (mEncoder == NULL) {
        return ERROR_UNSUPPORTED;
    }

    mOutputFormat = mInputFormat->dup();
    mOutputFormat->setString("mime", outputMIME.c_str());

    int32_t audioBitrate = getBitrate("media.wfd.audio-bitrate", 64000);
    int32_t videoBitrate = getBitrate("media.wfd.video-bitrate", 10000000);

    ALOGI("using audio bitrate of %d bps, video bitrate of %d bps",
          audioBitrate, videoBitrate);

    if (isAudio) {
        mOutputFormat->setInt32("bitrate", audioBitrate);
    } else {
        mOutputFormat->setInt32("bitrate", videoBitrate);
        mOutputFormat->setInt32("frame-rate", 30);
        mOutputFormat->setInt32("i-frame-interval", 3);  // Iframes every 3 secs
    }

    ALOGV("output format is '%s'", mOutputFormat->debugString(0).c_str());

    status_t err = mEncoder->configure(
            mOutputFormat,
            NULL /* nativeWindow */,
            NULL /* crypto */,
            MediaCodec::CONFIGURE_FLAG_ENCODE);

    if (err != OK) {
        return err;
    }

    err = mEncoder->start();

    if (err != OK) {
        return err;
    }

    err = mEncoder->getInputBuffers(&mEncoderInputBuffers);

    if (err != OK) {
        return err;
    }

    return mEncoder->getOutputBuffers(&mEncoderOutputBuffers);
}

void Converter::feedAccessUnit(const sp<ABuffer> &accessUnit) {
    sp<AMessage> msg = new AMessage(kWhatFeedAccessUnit, id());
    msg->setBuffer("accessUnit", accessUnit);
    msg->post();
}

void Converter::signalEOS() {
    (new AMessage(kWhatInputEOS, id()))->post();
}

void Converter::notifyError(status_t err) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatError);
    notify->setInt32("err", err);
    notify->post();
}

void Converter::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatFeedAccessUnit:
        {
            sp<ABuffer> accessUnit;
            CHECK(msg->findBuffer("accessUnit", &accessUnit));

            mInputBufferQueue.push_back(accessUnit);

            feedEncoderInputBuffers();

            scheduleDoMoreWork();
            break;
        }

        case kWhatInputEOS:
        {
            mInputBufferQueue.push_back(NULL);

            feedEncoderInputBuffers();

            scheduleDoMoreWork();
            break;
        }

        case kWhatDoMoreWork:
        {
            mDoMoreWorkPending = false;
            status_t err = doMoreWork();

            if (err != OK) {
                notifyError(err);
            } else {
                scheduleDoMoreWork();
            }
            break;
        }

        default:
            TRESPASS();
    }
}

void Converter::scheduleDoMoreWork() {
    if (mDoMoreWorkPending) {
        return;
    }

    mDoMoreWorkPending = true;
    (new AMessage(kWhatDoMoreWork, id()))->post(1000ll);
}

status_t Converter::feedEncoderInputBuffers() {
    while (!mInputBufferQueue.empty()
            && !mAvailEncoderInputIndices.empty()) {
        sp<ABuffer> buffer = *mInputBufferQueue.begin();
        mInputBufferQueue.erase(mInputBufferQueue.begin());

        size_t bufferIndex = *mAvailEncoderInputIndices.begin();
        mAvailEncoderInputIndices.erase(mAvailEncoderInputIndices.begin());

        int64_t timeUs = 0ll;
        uint32_t flags = 0;

        if (buffer != NULL) {
            CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

            memcpy(mEncoderInputBuffers.itemAt(bufferIndex)->data(),
                   buffer->data(),
                   buffer->size());

            void *mediaBuffer;
            if (buffer->meta()->findPointer("mediaBuffer", &mediaBuffer)
                    && mediaBuffer != NULL) {
                mEncoderInputBuffers.itemAt(bufferIndex)->meta()
                    ->setPointer("mediaBuffer", mediaBuffer);

                buffer->meta()->setPointer("mediaBuffer", NULL);
            }
        } else {
            flags = MediaCodec::BUFFER_FLAG_EOS;
        }

        status_t err = mEncoder->queueInputBuffer(
                bufferIndex, 0, (buffer == NULL) ? 0 : buffer->size(),
                timeUs, flags);

        if (err != OK) {
            return err;
        }
    }

    return OK;
}

status_t Converter::doMoreWork() {
    size_t bufferIndex;
    status_t err = mEncoder->dequeueInputBuffer(&bufferIndex);

    if (err == OK) {
        mAvailEncoderInputIndices.push_back(bufferIndex);
        feedEncoderInputBuffers();
    }

    size_t offset;
    size_t size;
    int64_t timeUs;
    uint32_t flags;
    err = mEncoder->dequeueOutputBuffer(
            &bufferIndex, &offset, &size, &timeUs, &flags);

    if (err == OK) {
        if (flags & MediaCodec::BUFFER_FLAG_EOS) {
            sp<AMessage> notify = mNotify->dup();
            notify->setInt32("what", kWhatEOS);
            notify->post();
        } else {
            sp<ABuffer> buffer = new ABuffer(size);
            buffer->meta()->setInt64("timeUs", timeUs);

            memcpy(buffer->data(),
                   mEncoderOutputBuffers.itemAt(bufferIndex)->base() + offset,
                   size);

            if (flags & MediaCodec::BUFFER_FLAG_CODECCONFIG) {
                mOutputFormat->setBuffer("csd-0", buffer);
            } else {
                sp<AMessage> notify = mNotify->dup();
                notify->setInt32("what", kWhatAccessUnit);
                notify->setBuffer("accessUnit", buffer);
                notify->post();
            }
        }

        err = mEncoder->releaseOutputBuffer(bufferIndex);
    } else if (err == -EAGAIN) {
        err = OK;
    }

    return err;
}

}  // namespace android

