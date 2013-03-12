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
#define LOG_TAG "TunnelRenderer"
#include <utils/Log.h>

#include "TunnelRenderer.h"

#include "ATSParser.h"

#include <binder/IMemory.h>
#include <binder/IServiceManager.h>
#include <gui/SurfaceComposerClient.h>
#include <media/IMediaPlayerService.h>
#include <media/IStreamSource.h>
#include <media/mediaplayer.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <ui/DisplayInfo.h>

namespace android {

struct TunnelRenderer::PlayerClient : public BnMediaPlayerClient {
    PlayerClient() {}

    virtual void notify(int msg, int ext1, int ext2, const Parcel *obj) {
        ALOGI("notify %d, %d, %d", msg, ext1, ext2);
    }

protected:
    virtual ~PlayerClient() {}

private:
    DISALLOW_EVIL_CONSTRUCTORS(PlayerClient);
};

struct TunnelRenderer::StreamSource : public BnStreamSource {
    StreamSource(TunnelRenderer *owner);

    virtual void setListener(const sp<IStreamListener> &listener);
    virtual void setBuffers(const Vector<sp<IMemory> > &buffers);

    virtual void onBufferAvailable(size_t index);

    virtual uint32_t flags() const;

    void doSomeWork();

    void setTimeOffset(int64_t offset);

protected:
    virtual ~StreamSource();

private:
    mutable Mutex mLock;

    TunnelRenderer *mOwner;

    sp<IStreamListener> mListener;

    Vector<sp<IMemory> > mBuffers;
    List<size_t> mIndicesAvailable;

    size_t mNumDeqeued;

    int64_t mTimeOffsetUs;
    bool mTimeOffsetChanged;

    DISALLOW_EVIL_CONSTRUCTORS(StreamSource);
};

////////////////////////////////////////////////////////////////////////////////

TunnelRenderer::StreamSource::StreamSource(TunnelRenderer *owner)
    : mOwner(owner),
      mNumDeqeued(0),
      mTimeOffsetUs(0ll),
      mTimeOffsetChanged(false) {
}

TunnelRenderer::StreamSource::~StreamSource() {
}

void TunnelRenderer::StreamSource::setListener(
        const sp<IStreamListener> &listener) {
    mListener = listener;
}

void TunnelRenderer::StreamSource::setBuffers(
        const Vector<sp<IMemory> > &buffers) {
    mBuffers = buffers;
}

void TunnelRenderer::StreamSource::onBufferAvailable(size_t index) {
    CHECK_LT(index, mBuffers.size());

    {
        Mutex::Autolock autoLock(mLock);
        mIndicesAvailable.push_back(index);
    }

    doSomeWork();
}

uint32_t TunnelRenderer::StreamSource::flags() const {
    return kFlagAlignedVideoData | kFlagIsRealTimeData;
}

void TunnelRenderer::StreamSource::doSomeWork() {
    Mutex::Autolock autoLock(mLock);

    while (!mIndicesAvailable.empty()) {
        sp<ABuffer> srcBuffer = mOwner->dequeueBuffer();
        if (srcBuffer == NULL) {
            break;
        }

        ++mNumDeqeued;

        if (mTimeOffsetChanged) {
            sp<AMessage> extra = new AMessage;

            extra->setInt32(
                    IStreamListener::kKeyDiscontinuityMask,
                    ATSParser::DISCONTINUITY_TIME_OFFSET);

            extra->setInt64("offset", mTimeOffsetUs);

            mListener->issueCommand(
                    IStreamListener::DISCONTINUITY,
                    false /* synchronous */,
                    extra);

            mTimeOffsetChanged = false;
        }

        ALOGV("dequeue TS packet of size %d", srcBuffer->size());

        size_t index = *mIndicesAvailable.begin();
        mIndicesAvailable.erase(mIndicesAvailable.begin());

        sp<IMemory> mem = mBuffers.itemAt(index);
        CHECK_LE(srcBuffer->size(), mem->size());
        CHECK_EQ((srcBuffer->size() % 188), 0u);

        memcpy(mem->pointer(), srcBuffer->data(), srcBuffer->size());
        mListener->queueBuffer(index, srcBuffer->size());
    }
}

void TunnelRenderer::StreamSource::setTimeOffset(int64_t offset) {
    Mutex::Autolock autoLock(mLock);

    if (offset != mTimeOffsetUs) {
        mTimeOffsetUs = offset;
        mTimeOffsetChanged = true;
    }
}

////////////////////////////////////////////////////////////////////////////////

TunnelRenderer::TunnelRenderer(
        const sp<IGraphicBufferProducer> &bufferProducer)
    : mSurfaceTex(bufferProducer),
      mStartup(true) {
    mStreamSource = new StreamSource(this);
}

TunnelRenderer::~TunnelRenderer() {
    destroyPlayer();
}

void TunnelRenderer::setTimeOffset(int64_t offset) {
    mStreamSource->setTimeOffset(offset);
}

void TunnelRenderer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        default:
            TRESPASS();
    }
}

void TunnelRenderer::initPlayer() {
    if (mSurfaceTex == NULL) {
        mComposerClient = new SurfaceComposerClient;
        CHECK_EQ(mComposerClient->initCheck(), (status_t)OK);

        DisplayInfo info;
        SurfaceComposerClient::getDisplayInfo(0, &info);
        ssize_t displayWidth = info.w;
        ssize_t displayHeight = info.h;

        mSurfaceControl =
            mComposerClient->createSurface(
                    String8("A Surface"),
                    displayWidth,
                    displayHeight,
                    PIXEL_FORMAT_RGB_565,
                    0);

        CHECK(mSurfaceControl != NULL);
        CHECK(mSurfaceControl->isValid());

        SurfaceComposerClient::openGlobalTransaction();
        CHECK_EQ(mSurfaceControl->setLayer(INT_MAX), (status_t)OK);
        CHECK_EQ(mSurfaceControl->show(), (status_t)OK);
        SurfaceComposerClient::closeGlobalTransaction();

        mSurface = mSurfaceControl->getSurface();
        CHECK(mSurface != NULL);
    }

    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("media.player"));
    sp<IMediaPlayerService> service = interface_cast<IMediaPlayerService>(binder);
    CHECK(service.get() != NULL);

    mPlayerClient = new PlayerClient;

    mPlayer = service->create(mPlayerClient, 0);
    CHECK(mPlayer != NULL);
    CHECK_EQ(mPlayer->setDataSource(mStreamSource), (status_t)OK);

    mPlayer->setVideoSurfaceTexture(
            mSurfaceTex != NULL ? mSurfaceTex : mSurface->getIGraphicBufferProducer());

    mPlayer->start();
}

void TunnelRenderer::destroyPlayer() {
    mStreamSource.clear();

    mPlayer->setVideoSurfaceTexture(NULL);

    mPlayer->stop();
    mPlayer.clear();

    if (mSurfaceTex == NULL) {
        mSurface.clear();
        mSurfaceControl.clear();

        mComposerClient->dispose();
        mComposerClient.clear();
    }
}

void TunnelRenderer::queueBuffer(const sp<ABuffer> &buffer) {
    {
        Mutex::Autolock autoLock(mLock);
        mBuffers.push_back(buffer);
    }

    if (mStartup) {
        initPlayer();
        mStartup = false;
    }

    mStreamSource->doSomeWork();
}

sp<ABuffer> TunnelRenderer::dequeueBuffer() {
    Mutex::Autolock autoLock(mLock);
    if (mBuffers.empty()) {
        return NULL;
    }

    sp<ABuffer> buf = *mBuffers.begin();
    mBuffers.erase(mBuffers.begin());

    return buf;
}

}  // namespace android

