/*
 * Copyright 2014 The Android Open Source Project
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

#ifndef NUPLAYER_DECODER_H_
#define NUPLAYER_DECODER_H_

#include "NuPlayer.h"

#include "NuPlayerDecoderBase.h"

namespace android {

struct NuPlayer::Decoder : public DecoderBase {
    Decoder(const sp<AMessage> &notify,
            const sp<Source> &source,
            const sp<Renderer> &renderer = NULL,
            const sp<NativeWindowWrapper> &nativeWindow = NULL,
            const sp<CCDecoder> &ccDecoder = NULL);

    virtual void getStats(
            int64_t *mNumFramesTotal,
            int64_t *mNumFramesDropped) const;

protected:
    virtual ~Decoder();

    virtual void onMessageReceived(const sp<AMessage> &msg);

    virtual void onConfigure(const sp<AMessage> &format);
    virtual void onSetRenderer(const sp<Renderer> &renderer);
    virtual void onGetInputBuffers(Vector<sp<ABuffer> > *dstBuffers);
    virtual void onResume(bool notifyComplete);
    virtual void onFlush(bool notifyComplete);
    virtual void onShutdown(bool notifyComplete);
    virtual void doRequestBuffers();

private:
    enum {
        kWhatCodecNotify         = 'cdcN',
        kWhatRenderBuffer        = 'rndr',
    };

    sp<NativeWindowWrapper> mNativeWindow;

    sp<Source> mSource;
    sp<Renderer> mRenderer;
    sp<CCDecoder> mCCDecoder;

    sp<AMessage> mInputFormat;
    sp<AMessage> mOutputFormat;
    sp<MediaCodec> mCodec;
    sp<ALooper> mCodecLooper;

    List<sp<AMessage> > mPendingInputMessages;

    Vector<sp<ABuffer> > mInputBuffers;
    Vector<sp<ABuffer> > mOutputBuffers;
    Vector<sp<ABuffer> > mCSDsForCurrentFormat;
    Vector<sp<ABuffer> > mCSDsToSubmit;
    Vector<bool> mInputBufferIsDequeued;
    Vector<MediaBuffer *> mMediaBuffers;
    Vector<size_t> mDequeuedInputBuffers;

    int64_t mSkipRenderingUntilMediaTimeUs;
    int64_t mNumFramesTotal;
    int64_t mNumFramesDropped;
    bool mIsAudio;
    bool mIsVideoAVC;
    bool mIsSecure;
    bool mFormatChangePending;

    bool mPaused;
    bool mResumePending;
    AString mComponentName;

    bool handleAnInputBuffer();
    bool handleAnOutputBuffer();

    void releaseAndResetMediaBuffers();
    void requestCodecNotification();
    bool isStaleReply(const sp<AMessage> &msg);

    status_t fetchInputData(sp<AMessage> &reply);
    bool onInputBufferFetched(const sp<AMessage> &msg);
    void onRenderBuffer(const sp<AMessage> &msg);

    bool supportsSeamlessFormatChange(const sp<AMessage> &to) const;
    bool supportsSeamlessAudioFormatChange(const sp<AMessage> &targetFormat) const;
    void rememberCodecSpecificData(const sp<AMessage> &format);

    void notifyResumeCompleteIfNecessary();

    DISALLOW_EVIL_CONSTRUCTORS(Decoder);
};

}  // namespace android

#endif  // NUPLAYER_DECODER_H_
