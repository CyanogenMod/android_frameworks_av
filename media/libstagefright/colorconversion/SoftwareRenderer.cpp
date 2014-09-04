/*
 * Copyright (C) 2009 The Android Open Source Project
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

#define LOG_TAG "SoftwareRenderer"
#include <utils/Log.h>

#include "../include/SoftwareRenderer.h"

#include <cutils/properties.h> // for property_get
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <system/window.h>
#include <ui/GraphicBufferMapper.h>
#include <gui/IGraphicBufferProducer.h>

namespace android {

static bool runningInEmulator() {
    char prop[PROPERTY_VALUE_MAX];
    return (property_get("ro.kernel.qemu", prop, NULL) > 0);
}

static int ALIGN(int x, int y) {
    // y must be a power of 2.
    return (x + y - 1) & ~(y - 1);
}

SoftwareRenderer::SoftwareRenderer(const sp<ANativeWindow> &nativeWindow)
    : mColorFormat(OMX_COLOR_FormatUnused),
      mConverter(NULL),
      mYUVMode(None),
      mNativeWindow(nativeWindow),
      mWidth(0),
      mHeight(0),
      mCropLeft(0),
      mCropTop(0),
      mCropRight(0),
      mCropBottom(0),
      mCropWidth(0),
      mCropHeight(0) {
}

SoftwareRenderer::~SoftwareRenderer() {
    delete mConverter;
    mConverter = NULL;
}

void SoftwareRenderer::resetFormatIfChanged(const sp<AMessage> &format) {
    CHECK(format != NULL);

    int32_t colorFormatNew;
    CHECK(format->findInt32("color-format", &colorFormatNew));

    int32_t widthNew, heightNew;
    CHECK(format->findInt32("width", &widthNew));
    CHECK(format->findInt32("height", &heightNew));

    int32_t cropLeftNew, cropTopNew, cropRightNew, cropBottomNew;
    if (!format->findRect(
            "crop", &cropLeftNew, &cropTopNew, &cropRightNew, &cropBottomNew)) {
        cropLeftNew = cropTopNew = 0;
        cropRightNew = widthNew - 1;
        cropBottomNew = heightNew - 1;
    }

    if (static_cast<int32_t>(mColorFormat) == colorFormatNew &&
        mWidth == widthNew &&
        mHeight == heightNew &&
        mCropLeft == cropLeftNew &&
        mCropTop == cropTopNew &&
        mCropRight == cropRightNew &&
        mCropBottom == cropBottomNew) {
        // Nothing changed, no need to reset renderer.
        return;
    }

    mColorFormat = static_cast<OMX_COLOR_FORMATTYPE>(colorFormatNew);
    mWidth = widthNew;
    mHeight = heightNew;
    mCropLeft = cropLeftNew;
    mCropTop = cropTopNew;
    mCropRight = cropRightNew;
    mCropBottom = cropBottomNew;

    mCropWidth = mCropRight - mCropLeft + 1;
    mCropHeight = mCropBottom - mCropTop + 1;

    // by default convert everything to RGB565
    int halFormat = HAL_PIXEL_FORMAT_RGB_565;
    size_t bufWidth = mCropWidth;
    size_t bufHeight = mCropHeight;

    // hardware has YUV12 and RGBA8888 support, so convert known formats
    if (!runningInEmulator()) {
        switch (mColorFormat) {
            case OMX_COLOR_FormatYUV420Planar:
            case OMX_TI_COLOR_FormatYUV420PackedSemiPlanar:
            {
                halFormat = HAL_PIXEL_FORMAT_YV12;
                bufWidth = (mCropWidth + 1) & ~1;
                bufHeight = (mCropHeight + 1) & ~1;
                break;
            }
            case OMX_COLOR_Format24bitRGB888:
            {
                halFormat = HAL_PIXEL_FORMAT_RGB_888;
                bufWidth = (mCropWidth + 1) & ~1;
                bufHeight = (mCropHeight + 1) & ~1;
                break;
            }
            case OMX_COLOR_Format32bitARGB8888:
            case OMX_COLOR_Format32BitRGBA8888:
            {
                halFormat = HAL_PIXEL_FORMAT_RGBA_8888;
                bufWidth = (mCropWidth + 1) & ~1;
                bufHeight = (mCropHeight + 1) & ~1;
                break;
            }
            default:
            {
                break;
            }
        }
    }

    if (halFormat == HAL_PIXEL_FORMAT_RGB_565) {
        mConverter = new ColorConverter(
                mColorFormat, OMX_COLOR_Format16bitRGB565);
        CHECK(mConverter->isValid());
    }

    CHECK(mNativeWindow != NULL);
    CHECK(mCropWidth > 0);
    CHECK(mCropHeight > 0);
    CHECK(mConverter == NULL || mConverter->isValid());

    CHECK_EQ(0,
            native_window_set_usage(
            mNativeWindow.get(),
            GRALLOC_USAGE_SW_READ_NEVER | GRALLOC_USAGE_SW_WRITE_OFTEN
            | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP));

    CHECK_EQ(0,
            native_window_set_scaling_mode(
            mNativeWindow.get(),
            NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW));

    // Width must be multiple of 32???
    CHECK_EQ(0, native_window_set_buffers_dimensions(
                mNativeWindow.get(),
                bufWidth,
                bufHeight));
    CHECK_EQ(0, native_window_set_buffers_format(
                mNativeWindow.get(),
                halFormat));

    // NOTE: native window uses extended right-bottom coordinate
    android_native_rect_t crop;
    crop.left = mCropLeft;
    crop.top = mCropTop;
    crop.right = mCropRight + 1;
    crop.bottom = mCropBottom + 1;
    ALOGV("setting crop: [%d, %d, %d, %d] for size [%zu, %zu]",
          crop.left, crop.top, crop.right, crop.bottom, bufWidth, bufHeight);

    CHECK_EQ(0, native_window_set_crop(mNativeWindow.get(), &crop));

    int32_t rotationDegrees;
    if (!format->findInt32("rotation-degrees", &rotationDegrees)) {
        rotationDegrees = 0;
    }
    uint32_t transform;
    switch (rotationDegrees) {
        case 0: transform = 0; break;
        case 90: transform = HAL_TRANSFORM_ROT_90; break;
        case 180: transform = HAL_TRANSFORM_ROT_180; break;
        case 270: transform = HAL_TRANSFORM_ROT_270; break;
        default: transform = 0; break;
    }

    CHECK_EQ(0, native_window_set_buffers_transform(
                mNativeWindow.get(), transform));
}

void SoftwareRenderer::render(
        const void *data, size_t /*size*/, int64_t timestampNs,
        void* /*platformPrivate*/, const sp<AMessage>& format) {
    resetFormatIfChanged(format);

    ANativeWindowBuffer *buf;
    int err;
    if ((err = native_window_dequeue_buffer_and_wait(mNativeWindow.get(),
            &buf)) != 0) {
        ALOGW("Surface::dequeueBuffer returned error %d", err);
        return;
    }

    GraphicBufferMapper &mapper = GraphicBufferMapper::get();

    Rect bounds(mCropWidth, mCropHeight);

    void *dst;
    CHECK_EQ(0, mapper.lock(
                buf->handle, GRALLOC_USAGE_SW_WRITE_OFTEN, bounds, &dst));

    // TODO move the other conversions also into ColorConverter, and
    // fix cropping issues (when mCropLeft/Top != 0 or mWidth != mCropWidth)
    if (mConverter) {
        mConverter->convert(
                data,
                mWidth, mHeight,
                mCropLeft, mCropTop, mCropRight, mCropBottom,
                dst,
                buf->stride, buf->height,
                0, 0, mCropWidth - 1, mCropHeight - 1);
    } else if (mColorFormat == OMX_COLOR_FormatYUV420Planar) {
        const uint8_t *src_y = (const uint8_t *)data;
        const uint8_t *src_u =
                (const uint8_t *)data + mWidth * mHeight;
        const uint8_t *src_v = src_u + (mWidth / 2 * mHeight / 2);

        uint8_t *dst_y = (uint8_t *)dst;
        size_t dst_y_size = buf->stride * buf->height;
        size_t dst_c_stride = ALIGN(buf->stride / 2, 16);
        size_t dst_c_size = dst_c_stride * buf->height / 2;
        uint8_t *dst_v = dst_y + dst_y_size;
        uint8_t *dst_u = dst_v + dst_c_size;

        for (int y = 0; y < mCropHeight; ++y) {
            memcpy(dst_y, src_y, mCropWidth);

            src_y += mWidth;
            dst_y += buf->stride;
        }

        for (int y = 0; y < (mCropHeight + 1) / 2; ++y) {
            memcpy(dst_u, src_u, (mCropWidth + 1) / 2);
            memcpy(dst_v, src_v, (mCropWidth + 1) / 2);

            src_u += mWidth / 2;
            src_v += mWidth / 2;
            dst_u += dst_c_stride;
            dst_v += dst_c_stride;
        }
    } else if (mColorFormat == OMX_TI_COLOR_FormatYUV420PackedSemiPlanar) {
        const uint8_t *src_y = (const uint8_t *)data;
        const uint8_t *src_uv = (const uint8_t *)data
                + mWidth * (mHeight - mCropTop / 2);

        uint8_t *dst_y = (uint8_t *)dst;

        size_t dst_y_size = buf->stride * buf->height;
        size_t dst_c_stride = ALIGN(buf->stride / 2, 16);
        size_t dst_c_size = dst_c_stride * buf->height / 2;
        uint8_t *dst_v = dst_y + dst_y_size;
        uint8_t *dst_u = dst_v + dst_c_size;

        for (int y = 0; y < mCropHeight; ++y) {
            memcpy(dst_y, src_y, mCropWidth);

            src_y += mWidth;
            dst_y += buf->stride;
        }

        for (int y = 0; y < (mCropHeight + 1) / 2; ++y) {
            size_t tmp = (mCropWidth + 1) / 2;
            for (size_t x = 0; x < tmp; ++x) {
                dst_u[x] = src_uv[2 * x];
                dst_v[x] = src_uv[2 * x + 1];
            }

            src_uv += mWidth;
            dst_u += dst_c_stride;
            dst_v += dst_c_stride;
        }
    } else if (mColorFormat == OMX_COLOR_Format24bitRGB888) {
        uint8_t* srcPtr = (uint8_t*)data;
        uint8_t* dstPtr = (uint8_t*)dst;

        for (size_t y = 0; y < (size_t)mCropHeight; ++y) {
            memcpy(dstPtr, srcPtr, mCropWidth * 3);
            srcPtr += mWidth * 3;
            dstPtr += buf->stride * 3;
        }
    } else if (mColorFormat == OMX_COLOR_Format32bitARGB8888) {
        uint8_t *srcPtr, *dstPtr;

        for (size_t y = 0; y < (size_t)mCropHeight; ++y) {
            srcPtr = (uint8_t*)data + mWidth * 4 * y;
            dstPtr = (uint8_t*)dst + buf->stride * 4 * y;
            for (size_t x = 0; x < (size_t)mCropWidth; ++x) {
                uint8_t a = *srcPtr++;
                for (size_t i = 0; i < 3; ++i) {   // copy RGB
                    *dstPtr++ = *srcPtr++;
                }
                *dstPtr++ = a;  // alpha last (ARGB to RGBA)
            }
        }
    } else if (mColorFormat == OMX_COLOR_Format32BitRGBA8888) {
        uint8_t* srcPtr = (uint8_t*)data;
        uint8_t* dstPtr = (uint8_t*)dst;

        for (size_t y = 0; y < (size_t)mCropHeight; ++y) {
            memcpy(dstPtr, srcPtr, mCropWidth * 4);
            srcPtr += mWidth * 4;
            dstPtr += buf->stride * 4;
        }
    } else {
        LOG_ALWAYS_FATAL("bad color format %#x", mColorFormat);
    }

    CHECK_EQ(0, mapper.unlock(buf->handle));

    if ((err = native_window_set_buffers_timestamp(mNativeWindow.get(),
            timestampNs)) != 0) {
        ALOGW("Surface::set_buffers_timestamp returned error %d", err);
    }

    if ((err = mNativeWindow->queueBuffer(mNativeWindow.get(), buf,
            -1)) != 0) {
        ALOGW("Surface::queueBuffer returned error %d", err);
    }
    buf = NULL;
}

}  // namespace android
