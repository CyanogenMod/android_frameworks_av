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

#ifndef NUPLAYER_DECODER_BASE_H_

#define NUPLAYER_DECODER_BASE_H_

#include "NuPlayer.h"

#include <media/stagefright/foundation/AHandler.h>

namespace android {

struct ABuffer;
struct MediaCodec;
struct MediaBuffer;

struct NuPlayer::DecoderBase : public AHandler {
    DecoderBase(const sp<AMessage> &notify);

    void configure(const sp<AMessage> &format);
    void init();

    void setRenderer(const sp<Renderer> &renderer);

    status_t getInputBuffers(Vector<sp<ABuffer> > *dstBuffers) const;
    void signalFlush();
    void signalResume(bool notifyComplete);
    void initiateShutdown();

    virtual void getStats(
            int64_t *mNumFramesTotal,
            int64_t *mNumFramesDropped) const = 0;

    enum {
        kWhatInputDiscontinuity  = 'inDi',
        kWhatVideoSizeChanged    = 'viSC',
        kWhatFlushCompleted      = 'flsC',
        kWhatShutdownCompleted   = 'shDC',
        kWhatResumeCompleted     = 'resC',
        kWhatEOS                 = 'eos ',
        kWhatError               = 'err ',
    };

protected:

    virtual ~DecoderBase();

    virtual void onMessageReceived(const sp<AMessage> &msg);

    virtual void onConfigure(const sp<AMessage> &format) = 0;
    virtual void onSetRenderer(const sp<Renderer> &renderer) = 0;
    virtual void onGetInputBuffers(Vector<sp<ABuffer> > *dstBuffers) = 0;
    virtual void onResume(bool notifyComplete) = 0;
    virtual void onFlush(bool notifyComplete) = 0;
    virtual void onShutdown(bool notifyComplete) = 0;

    void onRequestInputBuffers();
    void scheduleRequestBuffers();
    virtual void doRequestBuffers() = 0;
    virtual void handleError(int32_t err);

    sp<AMessage> mNotify;
    int32_t mBufferGeneration;

private:
    enum {
        kWhatConfigure           = 'conf',
        kWhatSetRenderer         = 'setR',
        kWhatGetInputBuffers     = 'gInB',
        kWhatRequestInputBuffers = 'reqB',
        kWhatFlush               = 'flus',
        kWhatShutdown            = 'shuD',
    };

    sp<ALooper> mDecoderLooper;
    bool mRequestInputBuffersPending;

    DISALLOW_EVIL_CONSTRUCTORS(DecoderBase);
};

}  // namespace android

#endif  // NUPLAYER_DECODER_BASE_H_
