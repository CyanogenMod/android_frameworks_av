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
#define LOG_TAG "DirectRenderer"
#include <utils/Log.h>

#include "DirectRenderer.h"

#include <gui/SurfaceComposerClient.h>
#include <gui/Surface.h>
#include <media/ICrypto.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>

namespace android {

DirectRenderer::DirectRenderer(
        const sp<IGraphicBufferProducer> &bufferProducer)
    : mSurfaceTex(bufferProducer),
      mVideoDecoderNotificationPending(false),
      mRenderPending(false),
      mFirstRenderTimeUs(-1ll),
      mFirstRenderRealUs(-1ll) {
}

DirectRenderer::~DirectRenderer() {
    if (mVideoDecoder != NULL) {
        mVideoDecoder->release();
        mVideoDecoder.clear();

        mVideoDecoderLooper->stop();
        mVideoDecoderLooper.clear();
    }
}

void DirectRenderer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatVideoDecoderNotify:
        {
            onVideoDecoderNotify();
            break;
        }

        case kWhatRender:
        {
            onRender();
            break;
        }

        default:
            TRESPASS();
    }
}

void DirectRenderer::setFormat(
        size_t trackIndex, const sp<AMessage> &format) {
    if (trackIndex == 1) {
        // Ignore audio for now.
        return;
    }

    CHECK(mVideoDecoder == NULL);

    AString mime;
    CHECK(format->findString("mime", &mime));

    mVideoDecoderLooper = new ALooper;
    mVideoDecoderLooper->setName("video codec looper");

    mVideoDecoderLooper->start(
            false /* runOnCallingThread */,
            false /* canCallJava */,
            PRIORITY_DEFAULT);

    mVideoDecoder = MediaCodec::CreateByType(
            mVideoDecoderLooper, mime.c_str(), false /* encoder */);

    CHECK(mVideoDecoder != NULL);

    status_t err = mVideoDecoder->configure(
            format,
            mSurfaceTex == NULL
                ? NULL : new Surface(mSurfaceTex),
            NULL /* crypto */,
            0 /* flags */);
    CHECK_EQ(err, (status_t)OK);

    err = mVideoDecoder->start();
    CHECK_EQ(err, (status_t)OK);

    err = mVideoDecoder->getInputBuffers(
            &mVideoDecoderInputBuffers);
    CHECK_EQ(err, (status_t)OK);

    scheduleVideoDecoderNotification();
}

void DirectRenderer::queueAccessUnit(
        size_t trackIndex, const sp<ABuffer> &accessUnit) {
    if (trackIndex == 1) {
        // Ignore audio for now.
        return;
    }

    if (mVideoDecoder == NULL) {
        sp<AMessage> format = new AMessage;
        format->setString("mime", "video/avc");
        format->setInt32("width", 640);
        format->setInt32("height", 360);

        setFormat(0, format);
    }

    mVideoAccessUnits.push_back(accessUnit);
    queueVideoDecoderInputBuffers();
}

void DirectRenderer::queueVideoDecoderInputBuffers() {
    if (mVideoDecoder == NULL) {
        return;
    }

    bool submittedMore = false;

    while (!mVideoAccessUnits.empty()
            && !mVideoDecoderInputBuffersAvailable.empty()) {
        size_t index = *mVideoDecoderInputBuffersAvailable.begin();

        mVideoDecoderInputBuffersAvailable.erase(
                mVideoDecoderInputBuffersAvailable.begin());

        sp<ABuffer> srcBuffer = *mVideoAccessUnits.begin();
        mVideoAccessUnits.erase(mVideoAccessUnits.begin());

        const sp<ABuffer> &dstBuffer =
            mVideoDecoderInputBuffers.itemAt(index);

        memcpy(dstBuffer->data(), srcBuffer->data(), srcBuffer->size());

        int64_t timeUs;
        CHECK(srcBuffer->meta()->findInt64("timeUs", &timeUs));

        status_t err = mVideoDecoder->queueInputBuffer(
                index,
                0 /* offset */,
                srcBuffer->size(),
                timeUs,
                0 /* flags */);
        CHECK_EQ(err, (status_t)OK);

        submittedMore = true;
    }

    if (submittedMore) {
        scheduleVideoDecoderNotification();
    }
}

void DirectRenderer::onVideoDecoderNotify() {
    mVideoDecoderNotificationPending = false;

    for (;;) {
        size_t index;
        status_t err = mVideoDecoder->dequeueInputBuffer(&index);

        if (err == OK) {
            mVideoDecoderInputBuffersAvailable.push_back(index);
        } else if (err == -EAGAIN) {
            break;
        } else {
            TRESPASS();
        }
    }

    queueVideoDecoderInputBuffers();

    for (;;) {
        size_t index;
        size_t offset;
        size_t size;
        int64_t timeUs;
        uint32_t flags;
        status_t err = mVideoDecoder->dequeueOutputBuffer(
                &index,
                &offset,
                &size,
                &timeUs,
                &flags);

        if (err == OK) {
            queueOutputBuffer(index, timeUs);
        } else if (err == INFO_OUTPUT_BUFFERS_CHANGED) {
            // We don't care.
        } else if (err == INFO_FORMAT_CHANGED) {
            // We don't care.
        } else if (err == -EAGAIN) {
            break;
        } else {
            TRESPASS();
        }
    }

    scheduleVideoDecoderNotification();
}

void DirectRenderer::queueOutputBuffer(size_t index, int64_t timeUs) {
#if 0
    OutputInfo info;
    info.mIndex = index;
    info.mTimeUs = timeUs;
    mOutputBuffers.push_back(info);

    scheduleRenderIfNecessary();
#else
    status_t err = mVideoDecoder->renderOutputBufferAndRelease(index);
    CHECK_EQ(err, (status_t)OK);
#endif
}

void DirectRenderer::scheduleRenderIfNecessary() {
    if (mRenderPending || mOutputBuffers.empty()) {
        return;
    }

    mRenderPending = true;

    int64_t timeUs = (*mOutputBuffers.begin()).mTimeUs;
    int64_t nowUs = ALooper::GetNowUs();

    if (mFirstRenderTimeUs < 0ll) {
        mFirstRenderTimeUs = timeUs;
        mFirstRenderRealUs = nowUs;
    }

    int64_t whenUs = timeUs - mFirstRenderTimeUs + mFirstRenderRealUs;
    int64_t delayUs = whenUs - nowUs;

    (new AMessage(kWhatRender, id()))->post(delayUs);
}

void DirectRenderer::onRender() {
    mRenderPending = false;

    int64_t nowUs = ALooper::GetNowUs();

    while (!mOutputBuffers.empty()) {
        const OutputInfo &info = *mOutputBuffers.begin();

        if (info.mTimeUs > nowUs) {
            break;
        }

        status_t err = mVideoDecoder->renderOutputBufferAndRelease(info.mIndex);
        CHECK_EQ(err, (status_t)OK);

        mOutputBuffers.erase(mOutputBuffers.begin());
    }

    scheduleRenderIfNecessary();
}

void DirectRenderer::scheduleVideoDecoderNotification() {
    if (mVideoDecoderNotificationPending) {
        return;
    }

    sp<AMessage> notify =
        new AMessage(kWhatVideoDecoderNotify, id());

    mVideoDecoder->requestActivityNotification(notify);
    mVideoDecoderNotificationPending = true;
}

}  // namespace android

