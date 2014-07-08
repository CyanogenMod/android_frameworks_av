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

//#define LOG_NDEBUG 0
#define LOG_TAG "NuPlayerDecoderPassThrough"
#include <utils/Log.h>
#include <inttypes.h>

#include "NuPlayerDecoderPassThrough.h"

#include <media/ICrypto.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>

namespace android {

static const int kMaxPendingBuffers = 10;

NuPlayer::DecoderPassThrough::DecoderPassThrough(
        const sp<AMessage> &notify)
    : Decoder(notify),
      mNotify(notify),
      mBufferGeneration(0),
      mReachedEOS(true),
      mPendingBuffers(0),
      mComponentName("pass through decoder") {
    mDecoderLooper = new ALooper;
    mDecoderLooper->setName("NuPlayerDecoderPassThrough");
    mDecoderLooper->start(false, false, ANDROID_PRIORITY_AUDIO);
}

NuPlayer::DecoderPassThrough::~DecoderPassThrough() {
}

void NuPlayer::DecoderPassThrough::configure(const sp<AMessage> &format) {
    sp<AMessage> msg = new AMessage(kWhatConfigure, id());
    msg->setMessage("format", format);
    msg->post();
}

void NuPlayer::DecoderPassThrough::init() {
    mDecoderLooper->registerHandler(this);
}

void NuPlayer::DecoderPassThrough::signalFlush() {
    (new AMessage(kWhatFlush, id()))->post();
}

void NuPlayer::DecoderPassThrough::signalResume() {
    (new AMessage(kWhatResume, id()))->post();
}

void NuPlayer::DecoderPassThrough::initiateShutdown() {
    (new AMessage(kWhatShutdown, id()))->post();
}

bool NuPlayer::DecoderPassThrough::supportsSeamlessFormatChange(
        const sp<AMessage> & /* targetFormat */) const {
    return true;
}

void NuPlayer::DecoderPassThrough::onConfigure(const sp<AMessage> &format) {
    ALOGV("[%s] onConfigure", mComponentName.c_str());
    mPendingBuffers = 0;
    mReachedEOS = false;
    ++mBufferGeneration;

    requestABuffer();

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatOutputFormatChanged);
    notify->setMessage("format", format);
    notify->post();
}

bool NuPlayer::DecoderPassThrough::isStaleReply(const sp<AMessage> &msg) {
    int32_t generation;
    CHECK(msg->findInt32("generation", &generation));
    return generation != mBufferGeneration;
}

void NuPlayer::DecoderPassThrough::requestABuffer() {
    if (mPendingBuffers >= kMaxPendingBuffers || mReachedEOS) {
        ALOGV("[%s] mReachedEOS=%d, max pending buffers(%d:%d)",
                mComponentName.c_str(), (mReachedEOS ? 1 : 0),
                mPendingBuffers, kMaxPendingBuffers);
        return;
    }

    sp<AMessage> reply = new AMessage(kWhatInputBufferFilled, id());
    reply->setInt32("generation", mBufferGeneration);

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatFillThisBuffer);
    notify->setMessage("reply", reply);
    notify->post();
    mPendingBuffers++;

    sp<AMessage> message = new AMessage(kWhatRequestABuffer, id());
    message->setInt32("generation", mBufferGeneration);
    message->post();
    return;
}

void android::NuPlayer::DecoderPassThrough::onInputBufferFilled(
        const sp<AMessage> &msg) {
    if (mReachedEOS) {
        return;
    }

    sp<ABuffer> buffer;
    msg->findBuffer("buffer", &buffer);
    if (buffer == NULL) {
        mReachedEOS = true;

        sp<AMessage> notify = mNotify->dup();
        notify->setInt32("what", kWhatEOS);
        notify->setInt32("err", ERROR_END_OF_STREAM);
        notify->post();
        return;
    }

    sp<AMessage> reply = new AMessage(kWhatBufferConsumed, id());
    reply->setInt32("generation", mBufferGeneration);

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatDrainThisBuffer);
    notify->setBuffer("buffer", buffer);
    notify->setMessage("reply", reply);
    notify->post();
}

void NuPlayer::DecoderPassThrough::onBufferConsumed() {
    mPendingBuffers--;
    sp<AMessage> message = new AMessage(kWhatRequestABuffer, id());
    message->setInt32("generation", mBufferGeneration);
    message->post();
}

void NuPlayer::DecoderPassThrough::onFlush() {
    ++mBufferGeneration;

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatFlushCompleted);
    notify->post();
    mPendingBuffers = 0;
    mReachedEOS = false;
}

void NuPlayer::DecoderPassThrough::onShutdown() {
    ++mBufferGeneration;

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatShutdownCompleted);
    notify->post();
    mReachedEOS = true;
}

void NuPlayer::DecoderPassThrough::onMessageReceived(const sp<AMessage> &msg) {
    ALOGV("[%s] onMessage: %s", mComponentName.c_str(),
            msg->debugString().c_str());

    switch (msg->what()) {
        case kWhatConfigure:
        {
            sp<AMessage> format;
            CHECK(msg->findMessage("format", &format));
            onConfigure(format);
            break;
        }

        case kWhatRequestABuffer:
        {
            if (!isStaleReply(msg)) {
                requestABuffer();
            }

            break;
        }

        case kWhatInputBufferFilled:
        {
            if (!isStaleReply(msg)) {
                onInputBufferFilled(msg);
            }
            break;
        }

        case kWhatBufferConsumed:
        {
            if (!isStaleReply(msg)) {
                onBufferConsumed();
            }
            break;
        }

        case kWhatFlush:
        {
            onFlush();
            break;
        }

        case kWhatResume:
        {
            requestABuffer();
            break;
        }

        case kWhatShutdown:
        {
            onShutdown();
            break;
        }

        default:
            TRESPASS();
            break;
    }
}

}  // namespace android
