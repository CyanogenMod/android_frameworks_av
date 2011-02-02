/*
 * Copyright (C) 2011 NXP Software
 * Copyright (C) 2011 The Android Open Source Project
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


#define LOG_NDEBUG 1
#define LOG_TAG "PreviewRenderer"
#include <utils/Log.h>

#include "PreviewRenderer.h"

#include <binder/MemoryHeapBase.h>
#include <binder/MemoryHeapPmem.h>
#include <media/stagefright/MediaDebug.h>
#include <surfaceflinger/Surface.h>

namespace android {

PreviewRenderer::PreviewRenderer(
        OMX_COLOR_FORMATTYPE colorFormat,
        const sp<Surface> &surface,
        size_t displayWidth, size_t displayHeight,
        size_t decodedWidth, size_t decodedHeight,
        int32_t rotationDegrees)
    : mColorFormat(colorFormat),
      mConverter(NULL),
      mYUVMode(None),
      mSurface(surface),
      mDisplayWidth(displayWidth),
      mDisplayHeight(displayHeight),
      mDecodedWidth(decodedWidth),
      mDecodedHeight(decodedHeight) {
    LOGV("input format = %d", mColorFormat);
    LOGV("display = %d x %d, decoded = %d x %d",
            mDisplayWidth, mDisplayHeight, mDecodedWidth, mDecodedHeight);

    mDecodedWidth = mDisplayWidth;
    mDecodedHeight = mDisplayHeight;

    int halFormat;
    switch (mColorFormat) {
        case OMX_COLOR_FormatYUV420Planar:
        {
            halFormat = HAL_PIXEL_FORMAT_YV12;
            mYUVMode = None;
            break;
        }
        default:
            halFormat = HAL_PIXEL_FORMAT_RGB_565;

            mConverter = new ColorConverter(
                    mColorFormat, OMX_COLOR_Format16bitRGB565);
            CHECK(mConverter->isValid());
            break;
    }

    CHECK(mSurface.get() != NULL);
    CHECK(mDecodedWidth > 0);
    CHECK(mDecodedHeight > 0);
    CHECK(mConverter == NULL || mConverter->isValid());

    CHECK_EQ(0,
            native_window_set_usage(
            mSurface.get(),
            GRALLOC_USAGE_SW_READ_NEVER | GRALLOC_USAGE_SW_WRITE_OFTEN
            | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP));

    CHECK_EQ(0, native_window_set_buffer_count(mSurface.get(), 3));

    // Width must be multiple of 32???
    CHECK_EQ(0, native_window_set_buffers_geometry(
                mSurface.get(), mDecodedWidth, mDecodedHeight,
                halFormat));

    uint32_t transform;
    switch (rotationDegrees) {
        case 0: transform = 0; break;
        case 90: transform = HAL_TRANSFORM_ROT_90; break;
        case 180: transform = HAL_TRANSFORM_ROT_180; break;
        case 270: transform = HAL_TRANSFORM_ROT_270; break;
        default: transform = 0; break;
    }

    if (transform) {
        CHECK_EQ(0, native_window_set_buffers_transform(
                    mSurface.get(), transform));
    }
}

PreviewRenderer::~PreviewRenderer() {
    delete mConverter;
    mConverter = NULL;
}


//
// Provides a buffer and associated stride
// This buffer is allocated by the SurfaceFlinger
//
// For optimal display performances, you should :
// 1) call getBufferYV12()
// 2) fill the buffer with your data
// 3) call renderYV12() to take these changes into account
//
// For each call to getBufferYV12(), you must also call renderYV12()
// Expected format in the buffer is YV12 formats (similar to YUV420 planar fromat)
// for more details on this YV12 cf hardware/libhardware/include/hardware/hardware.h
//
void PreviewRenderer::getBufferYV12(uint8_t **data, size_t *stride) {
    int err = OK;
    LOGV("getBuffer START");

    if ((err = mSurface->dequeueBuffer(mSurface.get(), &mBuf)) != 0) {
        LOGW("Surface::dequeueBuffer returned error %d", err);
        return;
    }

    CHECK_EQ(0, mSurface->lockBuffer(mSurface.get(), mBuf));

    GraphicBufferMapper &mapper = GraphicBufferMapper::get();

    Rect bounds(mDecodedWidth, mDecodedHeight);

    void *dst;
    CHECK_EQ(0, mapper.lock(
                mBuf->handle, GRALLOC_USAGE_SW_WRITE_OFTEN, bounds, &dst));
    LOGV("Buffer locked");

    *data   = (uint8_t*)dst;
    *stride = mBuf->stride;

    LOGV("getBuffer END %p %d", dst, mBuf->stride);
}


//
// Display the content of the buffer provided by last call to getBufferYV12()
//
// See getBufferYV12() for details.
//
void PreviewRenderer::renderYV12() {
    LOGV("renderYV12() START");
    int err = OK;

    GraphicBufferMapper &mapper = GraphicBufferMapper::get();

    if (mBuf!= NULL) {
        CHECK_EQ(0, mapper.unlock(mBuf->handle));

        if ((err = mSurface->queueBuffer(mSurface.get(), mBuf)) != 0) {
            LOGW("Surface::queueBuffer returned error %d", err);
        }
    }
    mBuf = NULL;
    LOGV("renderYV12() END");
}



//
// Display the given data buffer
// platformPrivate is not used (kept for backwrad compatibility)
// Please rather use getbuffer() and the other render()functions (with no params)
// for optimal display
//
void PreviewRenderer::render(
        const void *data, size_t size, void *platformPrivate) {
    android_native_buffer_t *buf;
    int err;

    if ((err = mSurface->dequeueBuffer(mSurface.get(), &buf)) != 0) {
        LOGW("Surface::dequeueBuffer returned error %d", err);
        return;
    }

    CHECK_EQ(0, mSurface->lockBuffer(mSurface.get(), buf));

    GraphicBufferMapper &mapper = GraphicBufferMapper::get();

    Rect bounds(mDecodedWidth, mDecodedHeight);

    void *dst;
    CHECK_EQ(0, mapper.lock(
                buf->handle, GRALLOC_USAGE_SW_WRITE_OFTEN, bounds, &dst));
    LOGV("Buffer locked");

    if (mConverter) {
        LOGV("Convert to RGB565");
        mConverter->convert(data,
                mDecodedWidth, mDecodedHeight,
                0,0,mDecodedWidth, mDecodedHeight,
                dst, mDecodedWidth, mDecodedHeight,
                0,0,mDecodedWidth, mDecodedHeight);
    } else if (mYUVMode == None) {
        // Input and output are both YUV420sp, but the alignment requirements
        // are different.
        LOGV("mYUVMode == None %d x %d", mDecodedWidth, mDecodedHeight);
        size_t srcYStride = mDecodedWidth;
        const uint8_t *srcY = (const uint8_t *)data;
        uint8_t *dstY = (uint8_t *)dst;
        LOGV("srcY =       %p   dstY =       %p", srcY, dstY);
        LOGV("srcYStride = %d   dstYstride = %d", srcYStride, buf->stride);
        for (size_t i = 0; i < mDecodedHeight; ++i) {
            memcpy(dstY, srcY, mDecodedWidth);
            srcY += srcYStride;
            dstY += buf->stride;
        }

        size_t srcUVStride = (mDecodedWidth + 1) / 2;
        size_t dstUVStride = ALIGN(mDecodedWidth / 2, 32);
        LOGV("srcUVStride = %d   dstUVStride = %d", srcUVStride, dstUVStride);

        // Copy V
        // Source buffer is YUV, skip U
        const uint8_t *srcV = (const uint8_t *)data
                + mDecodedHeight * mDecodedWidth + (mDecodedHeight * mDecodedWidth)/4;
        // Destination buffer is YVU
        uint8_t *dstUV = (uint8_t *)dst
                + buf->stride*mDecodedHeight;
        LOGV("srcV =       %p   dstUV =       %p", srcV, dstUV);
        for (size_t i = 0; i < (mDecodedHeight+1)/2; ++i) {
            memcpy(dstUV, srcV, mDecodedWidth/2);
            srcV += srcUVStride;
            dstUV += dstUVStride;
        }


        // Copy V
        // Source buffer is YUV, go back to end of Y
        const uint8_t *srcU = (const uint8_t *)data
            + mDecodedHeight * mDecodedWidth ;
        // Destination buffer is YVU
        // Keep writing after V buffer has been filled, U follows immediately
        LOGV("srcU =       %p   dstUV =       %p", srcU, dstUV);
        for (size_t i = 0; i < (mDecodedHeight+1)/2; ++i) {
            memcpy(dstUV, srcU, mDecodedWidth/2);
            srcU += srcUVStride;
            dstUV += dstUVStride;
        }
    } else {
        memcpy(dst, data, size);
    }

    CHECK_EQ(0, mapper.unlock(buf->handle));

    if ((err = mSurface->queueBuffer(mSurface.get(), buf)) != 0) {
        LOGW("Surface::queueBuffer returned error %d", err);
    }
    buf = NULL;
}

}  // namespace android
