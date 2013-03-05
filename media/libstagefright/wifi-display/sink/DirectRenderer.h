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
struct IGraphicBufferProducer;
struct MediaCodec;

// An experimental renderer that only supports video and decodes video data
// as soon as it arrives using a MediaCodec instance, rendering it without
// delay. Primarily meant to finetune packet loss discovery and minimize
// latency.
struct DirectRenderer : public AHandler {
    DirectRenderer(const sp<IGraphicBufferProducer> &bufferProducer);

    void setFormat(size_t trackIndex, const sp<AMessage> &format);
    void queueAccessUnit(size_t trackIndex, const sp<ABuffer> &accessUnit);

protected:
    virtual void onMessageReceived(const sp<AMessage> &msg);
    virtual ~DirectRenderer();

private:
    enum {
        kWhatVideoDecoderNotify,
        kWhatRender,
    };

    struct OutputInfo {
        size_t mIndex;
        int64_t mTimeUs;
    };

    sp<IGraphicBufferProducer> mSurfaceTex;

    sp<ALooper> mVideoDecoderLooper;
    sp<MediaCodec> mVideoDecoder;
    Vector<sp<ABuffer> > mVideoDecoderInputBuffers;
    List<size_t> mVideoDecoderInputBuffersAvailable;
    bool mVideoDecoderNotificationPending;

    List<sp<ABuffer> > mVideoAccessUnits;

    List<OutputInfo> mOutputBuffers;
    bool mRenderPending;
    int64_t mFirstRenderTimeUs;
    int64_t mFirstRenderRealUs;

    void onVideoDecoderNotify();
    void onRender();

    void queueVideoDecoderInputBuffers();
    void scheduleVideoDecoderNotification();
    void scheduleRenderIfNecessary();

    void queueOutputBuffer(size_t index, int64_t timeUs);

    DISALLOW_EVIL_CONSTRUCTORS(DirectRenderer);
};

}  // namespace android

#endif  // DIRECT_RENDERER_H_
