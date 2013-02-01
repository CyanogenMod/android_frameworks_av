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

#include "AnotherPacketSource.h"
#include "ATSParser.h"

#include <gui/SurfaceComposerClient.h>
#include <media/ICrypto.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>

namespace android {

#if 0
// static
const int64_t DirectRenderer::kPacketLostDelayUs = 80000ll;

// static
const int64_t DirectRenderer::kPacketLateDelayUs = 60000ll;
#else
// static
const int64_t DirectRenderer::kPacketLostDelayUs = 1000000ll;

// static
const int64_t DirectRenderer::kPacketLateDelayUs = -1ll;
#endif

DirectRenderer::DirectRenderer(
        const sp<AMessage> &notifyLost,
        const sp<IGraphicBufferProducer> &bufferProducer)
    : mNotifyLost(notifyLost),
      mSurfaceTex(bufferProducer),
      mTSParser(new ATSParser(ATSParser::ALIGNED_VIDEO_DATA)),
      mVideoDecoderNotificationPending(false),
      mAwaitingExtSeqNo(-1),
      mRequestedRetransmission(false),
      mPacketLostGeneration(0) {
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
        case kWhatQueueBuffer:
        {
            sp<ABuffer> buffer;
            CHECK(msg->findBuffer("buffer", &buffer));

            onQueueBuffer(buffer);

            dequeueMore();
            break;
        }

        case kWhatPacketLate:
        case kWhatPacketLost:
        {
            int32_t generation;
            CHECK(msg->findInt32("generation", &generation));

            if (generation != mPacketLostGeneration) {
                // stale.
                break;
            }

            if (msg->what() == kWhatPacketLate) {
                CHECK(!mRequestedRetransmission);
                CHECK_GE(mAwaitingExtSeqNo, 0);

                ALOGV("packet extSeqNo %d is late, requesting retransmission.",
                      mAwaitingExtSeqNo);

                sp<AMessage> notify = mNotifyLost->dup();
                notify->setInt32("seqNo", (mAwaitingExtSeqNo & 0xffff));
                notify->post();

                mRequestedRetransmission = true;
                break;
            }

            ALOGW("lost packet extSeqNo %d", mAwaitingExtSeqNo);

            sp<AMessage> extra;
            mTSParser->signalDiscontinuity(
                    ATSParser::DISCONTINUITY_TIME, extra);

            mAwaitingExtSeqNo = -1;
            mRequestedRetransmission = false;
            dequeueMore();
            break;
        }

        case kWhatVideoDecoderNotify:
        {
            onVideoDecoderNotify();
            break;
        }

        default:
            TRESPASS();
    }
}

void DirectRenderer::onQueueBuffer(const sp<ABuffer> &buffer) {
    int32_t newExtendedSeqNo = buffer->int32Data();

    if (mPackets.empty()) {
        mPackets.push_back(buffer);
        return;
    }

    if (mAwaitingExtSeqNo > 0 && newExtendedSeqNo < mAwaitingExtSeqNo) {
        // We're no longer interested in these. They're old.
        return;
    }

    List<sp<ABuffer> >::iterator firstIt = mPackets.begin();
    List<sp<ABuffer> >::iterator it = --mPackets.end();
    for (;;) {
        int32_t extendedSeqNo = (*it)->int32Data();

        if (extendedSeqNo == newExtendedSeqNo) {
            // Duplicate packet.
            return;
        }

        if (extendedSeqNo < newExtendedSeqNo) {
            // Insert new packet after the one at "it".
            mPackets.insert(++it, buffer);
            return;
        }

        if (it == firstIt) {
            // Insert new packet before the first existing one.
            mPackets.insert(it, buffer);
            return;
        }

        --it;
    }
}

void DirectRenderer::dequeueMore() {
    if (mAwaitingExtSeqNo >= 0) {
        // Remove all packets before the one we're looking for, they had
        // their chance.
        while (!mPackets.empty()
                && (*mPackets.begin())->int32Data() < mAwaitingExtSeqNo) {
            ALOGV("dropping late packet extSeqNo %d",
                  (*mPackets.begin())->int32Data());

            mPackets.erase(mPackets.begin());
        }
    }

    bool packetLostScheduled = (mAwaitingExtSeqNo >= 0);

    while (!mPackets.empty()) {
        sp<ABuffer> buffer = *mPackets.begin();
        int32_t extSeqNo = buffer->int32Data();

        if (mAwaitingExtSeqNo >= 0 && extSeqNo != mAwaitingExtSeqNo) {
            break;
        }

        mPackets.erase(mPackets.begin());

        if (packetLostScheduled) {
            packetLostScheduled = false;
            cancelPacketLost();
        }

        if (mRequestedRetransmission) {
            ALOGV("recovered after requesting retransmission of extSeqNo %d",
                  mAwaitingExtSeqNo);
        }

        CHECK_EQ(buffer->size() % 188, 0u);

        for (size_t offset = 0; offset < buffer->size(); offset += 188) {
            status_t err = mTSParser->feedTSPacket(
                    buffer->data() + offset, 188);

            CHECK_EQ(err, (status_t)OK);
        }

        mAwaitingExtSeqNo = extSeqNo + 1;
        mRequestedRetransmission = false;
    }

    if (!packetLostScheduled && mAwaitingExtSeqNo >= 0) {
        schedulePacketLost();
    }

    dequeueAccessUnits();
}

void DirectRenderer::dequeueAccessUnits() {
    sp<AnotherPacketSource> audioSource =
        static_cast<AnotherPacketSource *>(
                mTSParser->getSource(ATSParser::AUDIO).get());

    if (audioSource != NULL) {
        status_t finalResult;
        size_t n = 0;
        while (audioSource->hasBufferAvailable(&finalResult)) {
            sp<ABuffer> accessUnit;
            status_t err = audioSource->dequeueAccessUnit(&accessUnit);
            if (err == OK) {
                ++n;
            }
        }

        if (n > 0) {
            ALOGV("dequeued %d audio access units.", n);
        }
    }

    sp<AnotherPacketSource> videoSource =
        static_cast<AnotherPacketSource *>(
                mTSParser->getSource(ATSParser::VIDEO).get());

    if (videoSource != NULL) {
        if (mVideoDecoder == NULL) {
            sp<MetaData> meta = videoSource->getFormat();
            if (meta != NULL) {
                sp<AMessage> videoFormat;
                status_t err = convertMetaDataToMessage(meta, &videoFormat);
                CHECK_EQ(err, (status_t)OK);

                AString mime;
                CHECK(videoFormat->findString("mime", &mime));

                mVideoDecoderLooper = new ALooper;
                mVideoDecoderLooper->setName("video codec looper");

                mVideoDecoderLooper->start(
                        false /* runOnCallingThread */,
                        false /* canCallJava */,
                        PRIORITY_DEFAULT);

                mVideoDecoder = MediaCodec::CreateByType(
                        mVideoDecoderLooper, mime.c_str(), false /* encoder */);

                CHECK(mVideoDecoder != NULL);

                err = mVideoDecoder->configure(
                        videoFormat,
                        mSurfaceTex == NULL
                            ? NULL : new SurfaceTextureClient(mSurfaceTex),
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
        }

        status_t finalResult;
        size_t n = 0;
        while (videoSource->hasBufferAvailable(&finalResult)) {
            sp<ABuffer> accessUnit;
            status_t err = videoSource->dequeueAccessUnit(&accessUnit);
            if (err == OK) {
                mVideoAccessUnits.push_back(accessUnit);
                ++n;
            }
        }

        if (n > 0) {
            ALOGV("dequeued %d video access units.", n);
            queueVideoDecoderInputBuffers();
        }
    }
}

void DirectRenderer::schedulePacketLost() {
    sp<AMessage> msg;

    if (kPacketLateDelayUs > 0ll) {
        msg = new AMessage(kWhatPacketLate, id());
        msg->setInt32("generation", mPacketLostGeneration);
        msg->post(kPacketLateDelayUs);
    }

    msg = new AMessage(kWhatPacketLost, id());
    msg->setInt32("generation", mPacketLostGeneration);
    msg->post(kPacketLostDelayUs);
}

void DirectRenderer::cancelPacketLost() {
    ++mPacketLostGeneration;
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
            err = mVideoDecoder->renderOutputBufferAndRelease(index);
            CHECK_EQ(err, (status_t)OK);
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

