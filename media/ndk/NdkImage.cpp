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

#include <inttypes.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "NdkImage"

#include "NdkImagePriv.h"
#include "NdkImageReaderPriv.h"

#include <utils/Log.h>
#include "hardware/camera3.h"

using namespace android;

#define ALIGN(x, mask) ( ((x) + (mask) - 1) & ~((mask) - 1) )

AImage::AImage(AImageReader* reader, int32_t format,
        CpuConsumer::LockedBuffer* buffer, int64_t timestamp,
        int32_t width, int32_t height, int32_t numPlanes) :
        mReader(reader), mFormat(format),
        mBuffer(buffer), mTimestamp(timestamp),
        mWidth(width), mHeight(height), mNumPlanes(numPlanes) {
}

// Can only be called by free() with mLock hold
AImage::~AImage() {
    if (!mIsClosed) {
        LOG_ALWAYS_FATAL(
                "Error: AImage %p is deleted before returning buffer to AImageReader!", this);
    }
}

bool
AImage::isClosed() const {
    Mutex::Autolock _l(mLock);
    return mIsClosed;
}

void
AImage::close() {
    Mutex::Autolock _l(mLock);
    if (mIsClosed) {
        return;
    }
    sp<AImageReader> reader = mReader.promote();
    if (reader == nullptr) {
        LOG_ALWAYS_FATAL("Error: AImage not closed before AImageReader close!");
        return;
    }
    reader->releaseImageLocked(this);
    // Should have been set to nullptr in releaseImageLocked
    // Set to nullptr here for extra safety only
    mBuffer = nullptr;
    mIsClosed = true;
}

void
AImage::free() {
    if (!isClosed()) {
        ALOGE("Cannot free AImage before close!");
        return;
    }
    Mutex::Autolock _l(mLock);
    delete this;
}

void
AImage::lockReader() const {
    sp<AImageReader> reader = mReader.promote();
    if (reader == nullptr) {
        // Reader has been closed
        return;
    }
    reader->mLock.lock();
}

void
AImage::unlockReader() const {
    sp<AImageReader> reader = mReader.promote();
    if (reader == nullptr) {
        // Reader has been closed
        return;
    }
    reader->mLock.unlock();
}

media_status_t
AImage::getWidth(int32_t* width) const {
    if (width == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    *width = -1;
    if (isClosed()) {
        ALOGE("%s: image %p has been closed!", __FUNCTION__, this);
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    *width = mWidth;
    return AMEDIA_OK;
}

media_status_t
AImage::getHeight(int32_t* height) const {
    if (height == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    *height = -1;
    if (isClosed()) {
        ALOGE("%s: image %p has been closed!", __FUNCTION__, this);
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    *height = mHeight;
    return AMEDIA_OK;
}

media_status_t
AImage::getFormat(int32_t* format) const {
    if (format == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    *format = -1;
    if (isClosed()) {
        ALOGE("%s: image %p has been closed!", __FUNCTION__, this);
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    *format = mFormat;
    return AMEDIA_OK;
}

media_status_t
AImage::getNumPlanes(int32_t* numPlanes) const {
    if (numPlanes == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    *numPlanes = -1;
    if (isClosed()) {
        ALOGE("%s: image %p has been closed!", __FUNCTION__, this);
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    *numPlanes = mNumPlanes;
    return AMEDIA_OK;
}

media_status_t
AImage::getTimestamp(int64_t* timestamp) const {
    if (timestamp == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    *timestamp = -1;
    if (isClosed()) {
        ALOGE("%s: image %p has been closed!", __FUNCTION__, this);
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    *timestamp = mTimestamp;
    return AMEDIA_OK;
}

media_status_t
AImage::getPlanePixelStride(int planeIdx, /*out*/int32_t* pixelStride) const {
    if (planeIdx < 0 || planeIdx >= mNumPlanes) {
        ALOGE("Error: planeIdx %d out of bound [0,%d]",
                planeIdx, mNumPlanes - 1);
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    if (pixelStride == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    if (isClosed()) {
        ALOGE("%s: image %p has been closed!", __FUNCTION__, this);
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    int32_t fmt = mBuffer->flexFormat;
    switch (fmt) {
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
            *pixelStride = (planeIdx == 0) ? 1 : mBuffer->chromaStep;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            *pixelStride = (planeIdx == 0) ? 1 : 2;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_Y8:
            *pixelStride = 1;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_YV12:
            *pixelStride = 1;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_Y16:
        case HAL_PIXEL_FORMAT_RAW16:
        case HAL_PIXEL_FORMAT_RGB_565:
            // Single plane 16bpp data.
            *pixelStride = 2;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
            *pixelStride = 4;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_RGB_888:
            // Single plane, 24bpp.
            *pixelStride = 3;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_BLOB:
        case HAL_PIXEL_FORMAT_RAW10:
        case HAL_PIXEL_FORMAT_RAW12:
        case HAL_PIXEL_FORMAT_RAW_OPAQUE:
            // Blob is used for JPEG data, RAW10 and RAW12 is used for 10-bit and 12-bit raw data,
            // those are single plane data without pixel stride defined
            return AMEDIA_ERROR_UNSUPPORTED;
        default:
            ALOGE("Pixel format: 0x%x is unsupported", fmt);
            return AMEDIA_ERROR_UNSUPPORTED;
    }
}

media_status_t
AImage::getPlaneRowStride(int planeIdx, /*out*/int32_t* rowStride) const {
    if (planeIdx < 0 || planeIdx >= mNumPlanes) {
        ALOGE("Error: planeIdx %d out of bound [0,%d]",
                planeIdx, mNumPlanes - 1);
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    if (rowStride == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    if (isClosed()) {
        ALOGE("%s: image %p has been closed!", __FUNCTION__, this);
        return AMEDIA_ERROR_INVALID_OBJECT;
    }
    int32_t fmt = mBuffer->flexFormat;
    switch (fmt) {
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
            *rowStride = (planeIdx == 0) ? mBuffer->stride : mBuffer->chromaStride;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            *rowStride = mBuffer->width;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_YV12:
            if (mBuffer->stride % 16) {
                ALOGE("Stride %d is not 16 pixel aligned!", mBuffer->stride);
                return AMEDIA_ERROR_UNKNOWN;
            }
            *rowStride = (planeIdx == 0) ? mBuffer->stride : ALIGN(mBuffer->stride / 2, 16);
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_RAW10:
        case HAL_PIXEL_FORMAT_RAW12:
            // RAW10 and RAW12 are used for 10-bit and 12-bit raw data, they are single plane
            *rowStride = mBuffer->stride;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_Y8:
            if (mBuffer->stride % 16) {
                ALOGE("Stride %d is not 16 pixel aligned!", mBuffer->stride);
                return AMEDIA_ERROR_UNKNOWN;
            }
            *rowStride = mBuffer->stride;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_Y16:
        case HAL_PIXEL_FORMAT_RAW16:
            // In native side, strides are specified in pixels, not in bytes.
            // Single plane 16bpp bayer data. even width/height,
            // row stride multiple of 16 pixels (32 bytes)
            if (mBuffer->stride % 16) {
                ALOGE("Stride %d is not 16 pixel aligned!", mBuffer->stride);
                return AMEDIA_ERROR_UNKNOWN;
            }
            *rowStride = mBuffer->stride * 2;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_RGB_565:
            *rowStride = mBuffer->stride * 2;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
            *rowStride = mBuffer->stride * 4;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_RGB_888:
            // Single plane, 24bpp.
            *rowStride = mBuffer->stride * 3;
            return AMEDIA_OK;
        case HAL_PIXEL_FORMAT_BLOB:
        case HAL_PIXEL_FORMAT_RAW_OPAQUE:
            // Blob is used for JPEG/Raw opaque data. It is single plane and has 0 row stride and
            // no row stride defined
            return AMEDIA_ERROR_UNSUPPORTED;
        default:
            ALOGE("%s Pixel format: 0x%x is unsupported", __FUNCTION__, fmt);
          return AMEDIA_ERROR_UNSUPPORTED;
    }
}

uint32_t
AImage::getJpegSize() const {
    if (mBuffer == nullptr) {
        LOG_ALWAYS_FATAL("Error: buffer is null");
    }

    uint32_t size = 0;
    uint32_t width = mBuffer->width;
    uint8_t* jpegBuffer = mBuffer->data;

    // First check for JPEG transport header at the end of the buffer
    uint8_t* header = jpegBuffer + (width - sizeof(struct camera3_jpeg_blob));
    struct camera3_jpeg_blob* blob = (struct camera3_jpeg_blob*)(header);
    if (blob->jpeg_blob_id == CAMERA3_JPEG_BLOB_ID) {
        size = blob->jpeg_size;
        ALOGV("%s: Jpeg size = %d", __FUNCTION__, size);
    }

    // failed to find size, default to whole buffer
    if (size == 0) {
        /*
         * This is a problem because not including the JPEG header
         * means that in certain rare situations a regular JPEG blob
         * will be misidentified as having a header, in which case
         * we will get a garbage size value.
         */
        ALOGW("%s: No JPEG header detected, defaulting to size=width=%d",
                __FUNCTION__, width);
        size = width;
    }

    return size;
}

media_status_t
AImage::getPlaneData(int planeIdx,/*out*/uint8_t** data, /*out*/int* dataLength) const {
    if (planeIdx < 0 || planeIdx >= mNumPlanes) {
        ALOGE("Error: planeIdx %d out of bound [0,%d]",
                planeIdx, mNumPlanes - 1);
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    if (data == nullptr || dataLength == nullptr) {
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    if (isClosed()) {
        ALOGE("%s: image %p has been closed!", __FUNCTION__, this);
        return AMEDIA_ERROR_INVALID_OBJECT;
    }

    uint32_t dataSize, ySize, cSize, cStride;
    uint8_t* cb = nullptr;
    uint8_t* cr = nullptr;
    uint8_t* pData = nullptr;
    int bytesPerPixel = 0;
    int32_t fmt = mBuffer->flexFormat;

    switch (fmt) {
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
            pData = (planeIdx == 0) ? mBuffer->data :
                    (planeIdx == 1) ? mBuffer->dataCb : mBuffer->dataCr;
            // only map until last pixel
            if (planeIdx == 0) {
                dataSize = mBuffer->stride * (mBuffer->height - 1) + mBuffer->width;
            } else {
                dataSize = mBuffer->chromaStride * (mBuffer->height / 2 - 1) +
                        mBuffer->chromaStep * (mBuffer->width / 2 - 1) + 1;
            }
            break;
        // NV21
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            cr = mBuffer->data + (mBuffer->stride * mBuffer->height);
            cb = cr + 1;
            // only map until last pixel
            ySize = mBuffer->width * (mBuffer->height - 1) + mBuffer->width;
            cSize = mBuffer->width * (mBuffer->height / 2 - 1) + mBuffer->width - 1;

            pData = (planeIdx == 0) ? mBuffer->data :
                    (planeIdx == 1) ? cb : cr;
            dataSize = (planeIdx == 0) ? ySize : cSize;
            break;
        case HAL_PIXEL_FORMAT_YV12:
            // Y and C stride need to be 16 pixel aligned.
            if (mBuffer->stride % 16) {
                ALOGE("Stride %d is not 16 pixel aligned!", mBuffer->stride);
                return AMEDIA_ERROR_UNKNOWN;
            }

            ySize = mBuffer->stride * mBuffer->height;
            cStride = ALIGN(mBuffer->stride / 2, 16);
            cr = mBuffer->data + ySize;
            cSize = cStride * mBuffer->height / 2;
            cb = cr + cSize;

            pData = (planeIdx == 0) ? mBuffer->data :
                    (planeIdx == 1) ? cb : cr;
            dataSize = (planeIdx == 0) ? ySize : cSize;
            break;
        case HAL_PIXEL_FORMAT_Y8:
            // Single plane, 8bpp.

            pData = mBuffer->data;
            dataSize = mBuffer->stride * mBuffer->height;
            break;
        case HAL_PIXEL_FORMAT_Y16:
            bytesPerPixel = 2;

            pData = mBuffer->data;
            dataSize = mBuffer->stride * mBuffer->height * bytesPerPixel;
            break;
        case HAL_PIXEL_FORMAT_BLOB:
            // Used for JPEG data, height must be 1, width == size, single plane.
            if (mBuffer->height != 1) {
                ALOGE("Jpeg should have height value one but got %d", mBuffer->height);
                return AMEDIA_ERROR_UNKNOWN;
            }

            pData = mBuffer->data;
            dataSize = getJpegSize();
            break;
        case HAL_PIXEL_FORMAT_RAW16:
            // Single plane 16bpp bayer data.
            bytesPerPixel = 2;
            pData = mBuffer->data;
            dataSize = mBuffer->stride * mBuffer->height * bytesPerPixel;
            break;
        case HAL_PIXEL_FORMAT_RAW_OPAQUE:
            // Used for RAW_OPAQUE data, height must be 1, width == size, single plane.
            if (mBuffer->height != 1) {
                ALOGE("RAW_OPAQUE should have height value one but got %d", mBuffer->height);
                return AMEDIA_ERROR_UNKNOWN;
            }
            pData = mBuffer->data;
            dataSize = mBuffer->width;
            break;
        case HAL_PIXEL_FORMAT_RAW10:
            // Single plane 10bpp bayer data.
            if (mBuffer->width % 4) {
                ALOGE("Width is not multiple of 4 %d", mBuffer->width);
                return AMEDIA_ERROR_UNKNOWN;
            }
            if (mBuffer->height % 2) {
                ALOGE("Height is not multiple of 2 %d", mBuffer->height);
                return AMEDIA_ERROR_UNKNOWN;
            }
            if (mBuffer->stride < (mBuffer->width * 10 / 8)) {
                ALOGE("stride (%d) should be at least %d",
                        mBuffer->stride, mBuffer->width * 10 / 8);
                return AMEDIA_ERROR_UNKNOWN;
            }
            pData = mBuffer->data;
            dataSize = mBuffer->stride * mBuffer->height;
            break;
        case HAL_PIXEL_FORMAT_RAW12:
            // Single plane 10bpp bayer data.
            if (mBuffer->width % 4) {
                ALOGE("Width is not multiple of 4 %d", mBuffer->width);
                return AMEDIA_ERROR_UNKNOWN;
            }
            if (mBuffer->height % 2) {
                ALOGE("Height is not multiple of 2 %d", mBuffer->height);
                return AMEDIA_ERROR_UNKNOWN;
            }
            if (mBuffer->stride < (mBuffer->width * 12 / 8)) {
                ALOGE("stride (%d) should be at least %d",
                        mBuffer->stride, mBuffer->width * 12 / 8);
                return AMEDIA_ERROR_UNKNOWN;
            }
            pData = mBuffer->data;
            dataSize = mBuffer->stride * mBuffer->height;
            break;
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
            // Single plane, 32bpp.
            bytesPerPixel = 4;
            pData = mBuffer->data;
            dataSize = mBuffer->stride * mBuffer->height * bytesPerPixel;
            break;
        case HAL_PIXEL_FORMAT_RGB_565:
            // Single plane, 16bpp.
            bytesPerPixel = 2;
            pData = mBuffer->data;
            dataSize = mBuffer->stride * mBuffer->height * bytesPerPixel;
            break;
        case HAL_PIXEL_FORMAT_RGB_888:
            // Single plane, 24bpp.
            bytesPerPixel = 3;
            pData = mBuffer->data;
            dataSize = mBuffer->stride * mBuffer->height * bytesPerPixel;
            break;
        default:
            ALOGE("Pixel format: 0x%x is unsupported", fmt);
            return AMEDIA_ERROR_UNSUPPORTED;
    }

    *data = pData;
    *dataLength = dataSize;
    return AMEDIA_OK;
}

EXPORT
void AImage_delete(AImage* image) {
    ALOGV("%s", __FUNCTION__);
    if (image != nullptr) {
        image->lockReader();
        image->close();
        image->unlockReader();
        if (!image->isClosed()) {
            LOG_ALWAYS_FATAL("Image close failed!");
        }
        image->free();
    }
    return;
}

EXPORT
media_status_t AImage_getWidth(const AImage* image, /*out*/int32_t* width) {
    ALOGV("%s", __FUNCTION__);
    if (image == nullptr || width == nullptr) {
        ALOGE("%s: bad argument. image %p width %p",
                __FUNCTION__, image, width);
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    return image->getWidth(width);
}

EXPORT
media_status_t AImage_getHeight(const AImage* image, /*out*/int32_t* height) {
    ALOGV("%s", __FUNCTION__);
    if (image == nullptr || height == nullptr) {
        ALOGE("%s: bad argument. image %p height %p",
                __FUNCTION__, image, height);
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    return image->getHeight(height);
}

EXPORT
media_status_t AImage_getFormat(const AImage* image, /*out*/int32_t* format) {
    ALOGV("%s", __FUNCTION__);
    if (image == nullptr || format == nullptr) {
        ALOGE("%s: bad argument. image %p format %p",
                __FUNCTION__, image, format);
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    return image->getFormat(format);
}

EXPORT
media_status_t AImage_getCropRect(const AImage* image, /*out*/AImageCropRect* rect) {
    ALOGV("%s", __FUNCTION__);
    if (image == nullptr || rect == nullptr) {
        ALOGE("%s: bad argument. image %p rect %p",
                __FUNCTION__, image, rect);
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    // For now AImage only supports camera outputs where cropRect is always full window
    int32_t width = -1;
    media_status_t ret = image->getWidth(&width);
    if (ret != AMEDIA_OK) {
        return ret;
    }
    int32_t height = -1;
    ret = image->getHeight(&height);
    if (ret != AMEDIA_OK) {
        return ret;
    }
    rect->left = 0;
    rect->top = 0;
    rect->right = width;
    rect->bottom = height;
    return AMEDIA_OK;
}

EXPORT
media_status_t AImage_getTimestamp(const AImage* image, /*out*/int64_t* timestampNs) {
    ALOGV("%s", __FUNCTION__);
    if (image == nullptr || timestampNs == nullptr) {
        ALOGE("%s: bad argument. image %p timestampNs %p",
                __FUNCTION__, image, timestampNs);
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    return image->getTimestamp(timestampNs);
}

EXPORT
media_status_t AImage_getNumberOfPlanes(const AImage* image, /*out*/int32_t* numPlanes) {
    ALOGV("%s", __FUNCTION__);
    if (image == nullptr || numPlanes == nullptr) {
        ALOGE("%s: bad argument. image %p numPlanes %p",
                __FUNCTION__, image, numPlanes);
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    return image->getNumPlanes(numPlanes);
}

EXPORT
media_status_t AImage_getPlanePixelStride(
        const AImage* image, int planeIdx, /*out*/int32_t* pixelStride) {
    ALOGV("%s", __FUNCTION__);
    if (image == nullptr || pixelStride == nullptr) {
        ALOGE("%s: bad argument. image %p pixelStride %p",
                __FUNCTION__, image, pixelStride);
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    return image->getPlanePixelStride(planeIdx, pixelStride);
}

EXPORT
media_status_t AImage_getPlaneRowStride(
        const AImage* image, int planeIdx, /*out*/int32_t* rowStride) {
    ALOGV("%s", __FUNCTION__);
    if (image == nullptr || rowStride == nullptr) {
        ALOGE("%s: bad argument. image %p rowStride %p",
                __FUNCTION__, image, rowStride);
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    return image->getPlaneRowStride(planeIdx, rowStride);
}

EXPORT
media_status_t AImage_getPlaneData(
        const AImage* image, int planeIdx,
        /*out*/uint8_t** data, /*out*/int* dataLength) {
    ALOGV("%s", __FUNCTION__);
    if (image == nullptr || data == nullptr || dataLength == nullptr) {
        ALOGE("%s: bad argument. image %p data %p dataLength %p",
                __FUNCTION__, image, data, dataLength);
        return AMEDIA_ERROR_INVALID_PARAMETER;
    }
    return image->getPlaneData(planeIdx, data, dataLength);
}
