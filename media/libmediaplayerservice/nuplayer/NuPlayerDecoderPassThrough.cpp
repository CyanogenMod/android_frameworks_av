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

#include "NuPlayerRenderer.h"
#include "NuPlayerSource.h"

#include <media/ICrypto.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>

namespace android {

static const size_t kMaxCachedBytes = 200000;
// The buffers will contain a bit less than kAggregateBufferSizeBytes.
// So we can start off with just enough buffers to keep the cache full.
static const size_t kMaxPendingBuffers = 1 + (kMaxCachedBytes / NuPlayer::kAggregateBufferSizeBytes);

NuPlayer::DecoderPassThrough::DecoderPassThrough(
        const sp<AMessage> &notify,
        const sp<Source> &source,
        const sp<Renderer> &renderer)
    : Decoder(notify, source),
      mNotify(notify),
      mSource(source),
      mRenderer(renderer),
      mSkipRenderingUntilMediaTimeUs(-1ll),
      mBufferGeneration(0),
      mReachedEOS(true),
      mPendingBuffersToFill(0),
      mPendingBuffersToDrain(0),
      mCachedBytes(0),
      mComponentName("pass through decoder") {
    ALOGW_IF(renderer == NULL, "expect a non-NULL renderer");
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
    mCachedBytes = 0;
    mPendingBuffersToFill = 0;
    mPendingBuffersToDrain = 0;
    mReachedEOS = false;
    ++mBufferGeneration;

    requestMaxBuffers();

    uint32_t flags;
    int64_t durationUs;
    if (mSource->getDuration(&durationUs) == OK &&
            durationUs > AUDIO_SINK_MIN_DEEP_BUFFER_DURATION_US) {
        flags = AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
    } else {
        flags = AUDIO_OUTPUT_FLAG_NONE;
    }

    mRenderer->openAudioSink(
            format, true /* offloadOnly */, false /* hasVideo */, flags);
}

bool NuPlayer::DecoderPassThrough::isStaleReply(const sp<AMessage> &msg) {
    int32_t generation;
    CHECK(msg->findInt32("generation", &generation));
    return generation != mBufferGeneration;
}

bool NuPlayer::DecoderPassThrough::requestABuffer() {
    if (mCachedBytes >= kMaxCachedBytes) {
        ALOGV("[%s] mCachedBytes = %zu",
                mComponentName.c_str(), mCachedBytes);
        return false;
    }
    if (mReachedEOS) {
        ALOGV("[%s] reached EOS", mComponentName.c_str());
        return false;
    }

    sp<AMessage> reply = new AMessage(kWhatInputBufferFilled, id());
    reply->setInt32("generation", mBufferGeneration);

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatFillThisBuffer);
    notify->setMessage("reply", reply);
    notify->post();
    mPendingBuffersToFill++;
    ALOGV("requestABuffer: #ToFill = %zu, #ToDrain = %zu", mPendingBuffersToFill,
            mPendingBuffersToDrain);

    return true;
}

void android::NuPlayer::DecoderPassThrough::onInputBufferFilled(
        const sp<AMessage> &msg) {
    --mPendingBuffersToFill;
    if (mReachedEOS) {
        return;
    }

    sp<ABuffer> buffer;
    msg->findBuffer("buffer", &buffer);
    if (buffer == NULL) {
        mReachedEOS = true;
        if (mRenderer != NULL) {
            mRenderer->queueEOS(true /* audio */, ERROR_END_OF_STREAM);
        }
        return;
    }

    sp<AMessage> extra;
    if (buffer->meta()->findMessage("extra", &extra) && extra != NULL) {
        int64_t resumeAtMediaTimeUs;
        if (extra->findInt64(
                    "resume-at-mediatimeUs", &resumeAtMediaTimeUs)) {
            ALOGI("[%s] suppressing rendering until %lld us",
                    mComponentName.c_str(), (long long)resumeAtMediaTimeUs);
            mSkipRenderingUntilMediaTimeUs = resumeAtMediaTimeUs;
        }
    }

    int32_t bufferSize = buffer->size();
    mCachedBytes += bufferSize;

    if (mSkipRenderingUntilMediaTimeUs >= 0) {
        int64_t timeUs = 0;
        CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

        if (timeUs < mSkipRenderingUntilMediaTimeUs) {
            ALOGV("[%s] dropping buffer at time %lld as requested.",
                     mComponentName.c_str(), (long long)timeUs);

            onBufferConsumed(bufferSize);
            return;
        }

        mSkipRenderingUntilMediaTimeUs = -1;
    }

    if (mRenderer == NULL) {
        onBufferConsumed(bufferSize);
        return;
    }

    sp<AMessage> reply = new AMessage(kWhatBufferConsumed, id());
    reply->setInt32("generation", mBufferGeneration);
    reply->setInt32("size", bufferSize);

    mRenderer->queueBuffer(true /* audio */, buffer, reply);

    ++mPendingBuffersToDrain;
    ALOGV("onInputBufferFilled: #ToFill = %zu, #ToDrain = %zu, cachedBytes = %zu",
            mPendingBuffersToFill, mPendingBuffersToDrain, mCachedBytes);
}

void NuPlayer::DecoderPassThrough::onBufferConsumed(int32_t size) {
    --mPendingBuffersToDrain;
    mCachedBytes -= size;
    ALOGV("onBufferConsumed: #ToFill = %zu, #ToDrain = %zu, cachedBytes = %zu",
           mPendingBuffersToFill, mPendingBuffersToDrain, mCachedBytes);
    requestABuffer();
}

void NuPlayer::DecoderPassThrough::onFlush() {
    ++mBufferGeneration;
    mSkipRenderingUntilMediaTimeUs = -1;

    if (mRenderer != NULL) {
        mRenderer->flush(true /* audio */);
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatFlushCompleted);
    notify->post();
    mPendingBuffersToFill = 0;
    mPendingBuffersToDrain = 0;
    mCachedBytes = 0;
    mReachedEOS = false;
}

void NuPlayer::DecoderPassThrough::requestMaxBuffers() {
    for (size_t i = 0; i < kMaxPendingBuffers; i++) {
        if (!requestABuffer()) {
            break;
        }
    }
}

void NuPlayer::DecoderPassThrough::onShutdown() {
    ++mBufferGeneration;
    mSkipRenderingUntilMediaTimeUs = -1;

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
                int32_t size;
                CHECK(msg->findInt32("size", &size));
                onBufferConsumed(size);
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
            requestMaxBuffers();
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
