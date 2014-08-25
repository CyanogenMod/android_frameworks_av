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

#ifndef NUPLAYER_DECODER_H_

#define NUPLAYER_DECODER_H_

#include "NuPlayer.h"

#include <media/stagefright/foundation/AHandler.h>

namespace android {

struct ABuffer;
struct MediaCodec;
struct MediaBuffer;

struct NuPlayer::Decoder : public AHandler {
    Decoder(const sp<AMessage> &notify,
            const sp<NativeWindowWrapper> &nativeWindow = NULL);

    virtual void configure(const sp<AMessage> &format);
    virtual void init();

    status_t getInputBuffers(Vector<sp<ABuffer> > *dstBuffers) const;
    virtual void signalFlush(const sp<AMessage> &format = NULL);
    virtual void signalUpdateFormat(const sp<AMessage> &format);
    virtual void signalResume();
    virtual void initiateShutdown();

    virtual bool supportsSeamlessFormatChange(const sp<AMessage> &to) const;

    enum {
        kWhatFillThisBuffer      = 'flTB',
        kWhatDrainThisBuffer     = 'drTB',
        kWhatOutputFormatChanged = 'fmtC',
        kWhatFlushCompleted      = 'flsC',
        kWhatShutdownCompleted   = 'shDC',
        kWhatEOS                 = 'eos ',
        kWhatError               = 'err ',
    };

protected:

    virtual ~Decoder();

    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    enum {
        kWhatCodecNotify        = 'cdcN',
        kWhatConfigure          = 'conf',
        kWhatGetInputBuffers    = 'gInB',
        kWhatInputBufferFilled  = 'inpF',
        kWhatRenderBuffer       = 'rndr',
        kWhatFlush              = 'flus',
        kWhatShutdown           = 'shuD',
        kWhatUpdateFormat       = 'uFmt',
    };

    sp<AMessage> mNotify;
    sp<NativeWindowWrapper> mNativeWindow;

    sp<AMessage> mInputFormat;
    sp<AMessage> mOutputFormat;
    sp<MediaCodec> mCodec;
    sp<ALooper> mCodecLooper;
    sp<ALooper> mDecoderLooper;

    Vector<sp<ABuffer> > mInputBuffers;
    Vector<sp<ABuffer> > mOutputBuffers;
    Vector<sp<ABuffer> > mCSDsForCurrentFormat;
    Vector<sp<ABuffer> > mCSDsToSubmit;
    Vector<bool> mInputBufferIsDequeued;
    Vector<MediaBuffer *> mMediaBuffers;

    void handleError(int32_t err);
    bool handleAnInputBuffer();
    bool handleAnOutputBuffer();

    void releaseAndResetMediaBuffers();
    void requestCodecNotification();
    bool isStaleReply(const sp<AMessage> &msg);

    void onConfigure(const sp<AMessage> &format);
    void onFlush();
    void onResume();
    void onInputBufferFilled(const sp<AMessage> &msg);
    void onRenderBuffer(const sp<AMessage> &msg);
    void onShutdown();

    int32_t mBufferGeneration;
    bool mPaused;
    AString mComponentName;

    bool supportsSeamlessAudioFormatChange(const sp<AMessage> &targetFormat) const;
    void rememberCodecSpecificData(const sp<AMessage> &format);

    DISALLOW_EVIL_CONSTRUCTORS(Decoder);
};

struct NuPlayer::CCDecoder : public RefBase {
    enum {
        kWhatClosedCaptionData,
        kWhatTrackAdded,
    };

    CCDecoder(const sp<AMessage> &notify);

    size_t getTrackCount() const;
    sp<AMessage> getTrackInfo(size_t index) const;
    status_t selectTrack(size_t index, bool select);
    bool isSelected() const;
    void decode(const sp<ABuffer> &accessUnit);
    void display(int64_t timeUs);

private:
    struct CCData;

    sp<AMessage> mNotify;
    KeyedVector<int64_t, sp<ABuffer> > mCCMap;
    size_t mTrackCount;
    int32_t mSelectedTrack;

    bool isNullPad(CCData *cc) const;
    void dumpBytePair(const sp<ABuffer> &ccBuf) const;
    bool extractFromSEI(const sp<ABuffer> &accessUnit);

    DISALLOW_EVIL_CONSTRUCTORS(CCDecoder);
};

}  // namespace android

#endif  // NUPLAYER_DECODER_H_
