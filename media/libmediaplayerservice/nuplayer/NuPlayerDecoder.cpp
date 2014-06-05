/*
 * Copyright (C) 2010 The Android Open Source Project
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
#define LOG_TAG "NuPlayerDecoder"
#include <utils/Log.h>

#include "NuPlayerDecoder.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/ACodec.h>
#include <media/stagefright/MediaDefs.h>
#ifdef QCOM_HARDWARE
#include <media/stagefright/ExtendedCodec.h>
#endif

namespace android {

NuPlayer::Decoder::Decoder(
        const sp<AMessage> &notify,
        const sp<NativeWindowWrapper> &nativeWindow)
    : mNotify(notify),
      mNativeWindow(nativeWindow) {
}

NuPlayer::Decoder::~Decoder() {
}

void NuPlayer::Decoder::configure(const sp<AMessage> &format) {
    CHECK(mCodec == NULL);

    AString mime;
    CHECK(format->findString("mime", &mime));

    sp<AMessage> notifyMsg =
        new AMessage(kWhatCodecNotify, id());

    mCSDIndex = 0;
    for (size_t i = 0;; ++i) {
        sp<ABuffer> csd;
        if (!format->findBuffer(StringPrintf("csd-%d", i).c_str(), &csd)) {
            break;
        }

        mCSD.push(csd);
    }

#ifdef QCOM_HARDWARE
    sp<ABuffer> extendedCSD = ExtendedCodec::getRawCodecSpecificData(format);
    if (extendedCSD != NULL) {
        ALOGV("pushing extended CSD of size %d", extendedCSD->size());
        mCSD.push(extendedCSD);
    }

    sp<ABuffer> aacCSD = ExtendedCodec::getAacCodecSpecificData(format);
    if (aacCSD != NULL) {
        ALOGV("pushing AAC CSD of size %d", aacCSD->size());
        mCSD.push(aacCSD);
    }
#endif

    if (mNativeWindow != NULL) {
        format->setObject("native-window", mNativeWindow);
    }

    // Current video decoders do not return from OMX_FillThisBuffer
    // quickly, violating the OpenMAX specs, until that is remedied
    // we need to invest in an extra looper to free the main event
    // queue.
    bool needDedicatedLooper = !strncasecmp(mime.c_str(), "video/", 6);

    mFormat = format;
    mCodec = new ACodec;

    if (needDedicatedLooper && mCodecLooper == NULL) {
        mCodecLooper = new ALooper;
        mCodecLooper->setName("NuPlayerDecoder");
        mCodecLooper->start(false, false, ANDROID_PRIORITY_AUDIO);
    }

    (needDedicatedLooper ? mCodecLooper : looper())->registerHandler(mCodec);

    mCodec->setNotificationMessage(notifyMsg);
    mCodec->initiateSetup(format);
}

void NuPlayer::Decoder::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatCodecNotify:
        {
            int32_t what;
            CHECK(msg->findInt32("what", &what));

            if (what == ACodec::kWhatFillThisBuffer) {
                onFillThisBuffer(msg);
            } else {
                sp<AMessage> notify = mNotify->dup();
                notify->setMessage("codec-request", msg);
                notify->post();
            }
            break;
        }

        default:
            TRESPASS();
            break;
    }
}

void NuPlayer::Decoder::onFillThisBuffer(const sp<AMessage> &msg) {
    sp<AMessage> reply;
    CHECK(msg->findMessage("reply", &reply));

#if 0
    sp<ABuffer> outBuffer;
    CHECK(msg->findBuffer("buffer", &outBuffer));
#else
    sp<ABuffer> outBuffer;
#endif

    if (mCSDIndex < mCSD.size()) {
        outBuffer = mCSD.editItemAt(mCSDIndex++);
        outBuffer->meta()->setInt64("timeUs", 0);

        reply->setBuffer("buffer", outBuffer);
        reply->post();
        return;
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setMessage("codec-request", msg);
    notify->post();
}

void NuPlayer::Decoder::signalFlush() {
    if (mCodec != NULL) {
        mCodec->signalFlush();
    }
}

void NuPlayer::Decoder::signalResume() {
    if (mCodec != NULL) {
        mCodec->signalResume();
    }
}

void NuPlayer::Decoder::initiateShutdown() {
    if (mCodec != NULL) {
        mCodec->initiateShutdown();
    }
}

bool NuPlayer::Decoder::supportsSeamlessAudioFormatChange(const sp<AMessage> &targetFormat) const {
    if (targetFormat == NULL) {
        return true;
    }

    AString mime;
    if (!targetFormat->findString("mime", &mime)) {
        return false;
    }

    if (!strcasecmp(mime.c_str(), MEDIA_MIMETYPE_AUDIO_AAC)) {
        // field-by-field comparison
        const char * keys[] = { "channel-count", "sample-rate", "is-adts" };
        for (unsigned int i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            int32_t oldVal, newVal;
            if (!mFormat->findInt32(keys[i], &oldVal) || !targetFormat->findInt32(keys[i], &newVal)
                    || oldVal != newVal) {
                return false;
            }
        }

        sp<ABuffer> oldBuf, newBuf;
        if (mFormat->findBuffer("csd-0", &oldBuf) && targetFormat->findBuffer("csd-0", &newBuf)) {
            if (oldBuf->size() != newBuf->size()) {
                return false;
            }
            return !memcmp(oldBuf->data(), newBuf->data(), oldBuf->size());
        }
    }
    return false;
}

bool NuPlayer::Decoder::supportsSeamlessFormatChange(const sp<AMessage> &targetFormat) const {
    if (mFormat == NULL) {
        return false;
    }

    if (targetFormat == NULL) {
        return true;
    }

    AString oldMime, newMime;
    if (!mFormat->findString("mime", &oldMime)
            || !targetFormat->findString("mime", &newMime)
            || !(oldMime == newMime)) {
        return false;
    }

    bool audio = !strncasecmp(oldMime.c_str(), "audio/", strlen("audio/"));
    bool seamless;
    if (audio) {
        seamless = supportsSeamlessAudioFormatChange(targetFormat);
    } else {
        seamless = mCodec != NULL && mCodec->isConfiguredForAdaptivePlayback();
    }

    ALOGV("%s seamless support for %s", seamless ? "yes" : "no", oldMime.c_str());
    return seamless;
}

}  // namespace android

