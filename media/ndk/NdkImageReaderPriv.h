/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef _NDK_IMAGE_READER_PRIV_H
#define _NDK_IMAGE_READER_PRIV_H

#include <inttypes.h>

#include "NdkImageReader.h"

#include <utils/List.h>
#include <utils/Mutex.h>
#include <utils/StrongPointer.h>

#include <gui/CpuConsumer.h>
#include <gui/Surface.h>

#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AHandler.h>
#include <media/stagefright/foundation/AMessage.h>

using namespace android;

namespace {
    enum {
        IMAGE_READER_MAX_NUM_PLANES = 3,
    };

    enum {
        ACQUIRE_SUCCESS = 0,
        ACQUIRE_NO_BUFFERS = 1,
        ACQUIRE_MAX_IMAGES = 2,
    };
}

struct AImageReader : public RefBase {
  public:

    static bool isSupportedFormat(int32_t format);
    static int getNumPlanesForFormat(int32_t format);

    AImageReader(int32_t width, int32_t height, int32_t format, int32_t maxImages);
    ~AImageReader();

    // Inintialize AImageReader, uninitialized or failed to initialize AImageReader
    // should never be passed to application
    media_status_t init();

    media_status_t setImageListener(AImageReader_ImageListener* listener);

    media_status_t acquireNextImage(/*out*/AImage** image);
    media_status_t acquireLatestImage(/*out*/AImage** image);

    ANativeWindow* getWindow()    const { return mWindow.get(); };
    int32_t        getWidth()     const { return mWidth; };
    int32_t        getHeight()    const { return mHeight; };
    int32_t        getFormat()    const { return mFormat; };
    int32_t        getMaxImages() const { return mMaxImages; };


  private:

    friend struct AImage; // for grabing reader lock

    media_status_t acquireCpuConsumerImageLocked(/*out*/AImage** image);
    CpuConsumer::LockedBuffer* getLockedBufferLocked();
    void returnLockedBufferLocked(CpuConsumer::LockedBuffer* buffer);

    // Called by AImage to close image
    void releaseImageLocked(AImage* image);

    static int getBufferWidth(CpuConsumer::LockedBuffer* buffer);
    static int getBufferHeight(CpuConsumer::LockedBuffer* buffer);

    media_status_t setImageListenerLocked(AImageReader_ImageListener* listener);

    // definition of handler and message
    enum {
        kWhatImageAvailable
    };
    static const char* kCallbackFpKey;
    static const char* kContextKey;
    class CallbackHandler : public AHandler {
      public:
        CallbackHandler(AImageReader* reader) : mReader(reader) {}
        void onMessageReceived(const sp<AMessage> &msg) override;
      private:
        AImageReader* mReader;
    };
    sp<CallbackHandler> mHandler;
    sp<ALooper>         mCbLooper; // Looper thread where callbacks actually happen on

    List<CpuConsumer::LockedBuffer*> mBuffers;
    const int32_t mWidth;
    const int32_t mHeight;
    const int32_t mFormat;
    const int32_t mMaxImages;
    const int32_t mNumPlanes;

    struct FrameListener : public ConsumerBase::FrameAvailableListener {
      public:
        FrameListener(AImageReader* parent) : mReader(parent) {}

        void onFrameAvailable(const BufferItem& item) override;

        media_status_t setImageListener(AImageReader_ImageListener* listener);

      private:
        AImageReader_ImageListener mListener = {nullptr, nullptr};
        wp<AImageReader>           mReader;
        Mutex                      mLock;
    };
    sp<FrameListener> mFrameListener;

    int mHalFormat;
    android_dataspace mHalDataSpace;

    sp<IGraphicBufferProducer> mProducer;
    sp<Surface>                mSurface;
    sp<CpuConsumer>            mCpuConsumer;
    sp<ANativeWindow>          mWindow;

    List<AImage*>              mAcquiredImages;

    Mutex                      mLock;
};

#endif // _NDK_IMAGE_READER_PRIV_H
