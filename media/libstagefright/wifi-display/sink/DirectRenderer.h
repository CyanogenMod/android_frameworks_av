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

#ifndef DIRECT_RENDERER_H_

#define DIRECT_RENDERER_H_

#include <media/stagefright/foundation/AHandler.h>

namespace android {

struct ABuffer;
struct ATSParser;
struct IGraphicBufferProducer;
struct MediaCodec;

// An experimental renderer that only supports video and decodes video data
// as soon as it arrives using a MediaCodec instance, rendering it without
// delay. Primarily meant to finetune packet loss discovery and minimize
// latency.
struct DirectRenderer : public AHandler {
    DirectRenderer(
            const sp<AMessage> &notifyLost,
            const sp<IGraphicBufferProducer> &bufferProducer);

    enum {
        kWhatQueueBuffer = 'queB',
    };

protected:
    virtual void onMessageReceived(const sp<AMessage> &msg);
    virtual ~DirectRenderer();

private:
    enum {
        kWhatPacketLate,
        kWhatPacketLost,
        kWhatVideoDecoderNotify,
    };

    static const int64_t kPacketLateDelayUs;
    static const int64_t kPacketLostDelayUs;

    sp<AMessage> mNotifyLost;
    sp<IGraphicBufferProducer> mSurfaceTex;

    // Ordered by extended seq number.
    List<sp<ABuffer> > mPackets;

    sp<ATSParser> mTSParser;

    sp<ALooper> mVideoDecoderLooper;
    sp<MediaCodec> mVideoDecoder;
    Vector<sp<ABuffer> > mVideoDecoderInputBuffers;
    List<size_t> mVideoDecoderInputBuffersAvailable;
    bool mVideoDecoderNotificationPending;

    List<sp<ABuffer> > mVideoAccessUnits;

    int32_t mAwaitingExtSeqNo;
    bool mRequestedRetransmission;
    int32_t mPacketLostGeneration;

    void onQueueBuffer(const sp<ABuffer> &buffer);
    void onVideoDecoderNotify();

    void dequeueMore();
    void dequeueAccessUnits();

    void schedulePacketLost();
    void cancelPacketLost();

    void queueVideoDecoderInputBuffers();
    void scheduleVideoDecoderNotification();

    DISALLOW_EVIL_CONSTRUCTORS(DirectRenderer);
};

}  // namespace android

#endif  // DIRECT_RENDERER_H_
