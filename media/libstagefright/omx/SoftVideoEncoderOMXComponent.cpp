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

#include <inttypes.h>

//#define LOG_NDEBUG 0
#define LOG_TAG "SoftVideoEncoderOMXComponent"
#include <utils/Log.h>

#include "include/SoftVideoEncoderOMXComponent.h"

#include <hardware/gralloc.h>
#include <media/hardware/HardwareAPI.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaDefs.h>

#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>

namespace android {

SoftVideoEncoderOMXComponent::SoftVideoEncoderOMXComponent(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SimpleSoftOMXComponent(name, callbacks, appData, component),
      mGrallocModule(NULL) {
}

// static
void SoftVideoEncoderOMXComponent::ConvertFlexYUVToPlanar(
        uint8_t *dst, size_t dstStride, size_t dstVStride,
        struct android_ycbcr *ycbcr, int32_t width, int32_t height) {
    const uint8_t *src = (const uint8_t *)ycbcr->y;
    const uint8_t *srcU = (const uint8_t *)ycbcr->cb;
    const uint8_t *srcV = (const uint8_t *)ycbcr->cr;
    uint8_t *dstU = dst + dstVStride * dstStride;
    uint8_t *dstV = dstU + (dstVStride >> 1) * (dstStride >> 1);

    for (size_t y = height; y > 0; --y) {
        memcpy(dst, src, width);
        dst += dstStride;
        src += ycbcr->ystride;
    }
    if (ycbcr->cstride == ycbcr->ystride >> 1 && ycbcr->chroma_step == 1) {
        // planar
        for (size_t y = height >> 1; y > 0; --y) {
            memcpy(dstU, srcU, width >> 1);
            dstU += dstStride >> 1;
            srcU += ycbcr->cstride;
            memcpy(dstV, srcV, width >> 1);
            dstV += dstStride >> 1;
            srcV += ycbcr->cstride;
        }
    } else {
        // arbitrary
        for (size_t y = height >> 1; y > 0; --y) {
            for (size_t x = width >> 1; x > 0; --x) {
                *dstU++ = *srcU;
                *dstV++ = *srcV;
                srcU += ycbcr->chroma_step;
                srcV += ycbcr->chroma_step;
            }
            dstU += (dstStride >> 1) - (width >> 1);
            dstV += (dstStride >> 1) - (width >> 1);
            srcU += ycbcr->cstride - (width >> 1) * ycbcr->chroma_step;
            srcV += ycbcr->cstride - (width >> 1) * ycbcr->chroma_step;
        }
    }
}

// static
void SoftVideoEncoderOMXComponent::ConvertYUV420SemiPlanarToYUV420Planar(
        const uint8_t *inYVU, uint8_t* outYUV, int32_t width, int32_t height) {
    // TODO: add support for stride
    int32_t outYsize = width * height;
    uint32_t *outY  = (uint32_t *) outYUV;
    uint16_t *outCb = (uint16_t *) (outYUV + outYsize);
    uint16_t *outCr = (uint16_t *) (outYUV + outYsize + (outYsize >> 2));

    /* Y copying */
    memcpy(outY, inYVU, outYsize);

    /* U & V copying */
    // FIXME this only works if width is multiple of 4
    uint32_t *inYVU_4 = (uint32_t *) (inYVU + outYsize);
    for (int32_t i = height >> 1; i > 0; --i) {
        for (int32_t j = width >> 2; j > 0; --j) {
            uint32_t temp = *inYVU_4++;
            uint32_t tempU = temp & 0xFF;
            tempU = tempU | ((temp >> 8) & 0xFF00);

            uint32_t tempV = (temp >> 8) & 0xFF;
            tempV = tempV | ((temp >> 16) & 0xFF00);

            *outCb++ = tempU;
            *outCr++ = tempV;
        }
    }
}

// static
void SoftVideoEncoderOMXComponent::ConvertRGB32ToPlanar(
        uint8_t *dstY, size_t dstStride, size_t dstVStride,
        const uint8_t *src, size_t width, size_t height, size_t srcStride,
        bool bgr) {
    CHECK((width & 1) == 0);
    CHECK((height & 1) == 0);

    uint8_t *dstU = dstY + dstStride * dstVStride;
    uint8_t *dstV = dstU + (dstStride >> 1) * (dstVStride >> 1);

#ifdef SURFACE_IS_BGR32
    bgr = !bgr;
#endif

    const size_t redOffset   = bgr ? 2 : 0;
    const size_t greenOffset = 1;
    const size_t blueOffset  = bgr ? 0 : 2;

    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            unsigned red   = src[redOffset];
            unsigned green = src[greenOffset];
            unsigned blue  = src[blueOffset];

            // using ITU-R BT.601 conversion matrix
            unsigned luma =
                ((red * 66 + green * 129 + blue * 25) >> 8) + 16;

            dstY[x] = luma;

            if ((x & 1) == 0 && (y & 1) == 0) {
                unsigned U =
                    ((-red * 38 - green * 74 + blue * 112) >> 8) + 128;

                unsigned V =
                    ((red * 112 - green * 94 - blue * 18) >> 8) + 128;

                dstU[x >> 1] = U;
                dstV[x >> 1] = V;
            }
            src += 4;
        }

        if ((y & 1) == 0) {
            dstU += dstStride >> 1;
            dstV += dstStride >> 1;
        }

        src += srcStride - 4 * width;
        dstY += dstStride;
    }
}

const uint8_t *SoftVideoEncoderOMXComponent::extractGraphicBuffer(
        uint8_t *dst, size_t dstSize,
        const uint8_t *src, size_t srcSize,
        size_t width, size_t height) const {
    size_t dstStride = width;
    size_t dstVStride = height;

    MetadataBufferType bufferType = *(MetadataBufferType *)src;
    bool usingGraphicBuffer = bufferType == kMetadataBufferTypeGraphicBuffer;
    if (!usingGraphicBuffer && bufferType != kMetadataBufferTypeGrallocSource) {
        ALOGE("Unsupported metadata type (%d)", bufferType);
        return NULL;
    }

    if (mGrallocModule == NULL) {
        CHECK_EQ(0, hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &mGrallocModule));
    }

    const gralloc_module_t *grmodule =
        (const gralloc_module_t *)mGrallocModule;

    buffer_handle_t handle;
    int format;
    size_t srcStride;
    size_t srcVStride;
    if (usingGraphicBuffer) {
        if (srcSize < 4 + sizeof(GraphicBuffer *)) {
            ALOGE("Metadata is too small (%zu vs %zu)", srcSize, 4 + sizeof(GraphicBuffer *));
            return NULL;
        }

        GraphicBuffer *buffer = *(GraphicBuffer **)(src + 4);
        handle = buffer->handle;
        format = buffer->format;
        srcStride = buffer->stride;
        srcVStride = buffer->height;
        // convert stride from pixels to bytes
        if (format != HAL_PIXEL_FORMAT_YV12 &&
            format != HAL_PIXEL_FORMAT_YCbCr_420_888) {
            // TODO do we need to support other formats?
            srcStride *= 4;
        }
    } else {
        // TODO: remove this part.  Check if anyone uses this.

        if (srcSize < 4 + sizeof(buffer_handle_t)) {
            ALOGE("Metadata is too small (%zu vs %zu)", srcSize, 4 + sizeof(buffer_handle_t));
            return NULL;
        }

        handle = *(buffer_handle_t *)(src + 4);
        // assume HAL_PIXEL_FORMAT_RGBA_8888
        // there is no way to get the src stride without the graphic buffer
        format = HAL_PIXEL_FORMAT_RGBA_8888;
        srcStride = width * 4;
        srcVStride = height;
    }

    size_t neededSize =
        dstStride * dstVStride + (width >> 1)
                + (dstStride >> 1) * ((dstVStride >> 1) + (height >> 1) - 1);
    if (dstSize < neededSize) {
        ALOGE("destination buffer is too small (%zu vs %zu)", dstSize, neededSize);
        return NULL;
    }

    void *bits = NULL;
    struct android_ycbcr ycbcr;
    status_t res;
    if (format == HAL_PIXEL_FORMAT_YCbCr_420_888) {
        res = grmodule->lock_ycbcr(
                 grmodule, handle,
                 GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_NEVER,
                 0, 0, width, height, &ycbcr);
    } else {
        res = grmodule->lock(
                 grmodule, handle,
                 GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_NEVER,
                 0, 0, width, height, &bits);
    }
    if (res != OK) {
        ALOGE("Unable to lock image buffer %p for access", handle);
        return NULL;
    }

    switch (format) {
        case HAL_PIXEL_FORMAT_YV12:  // YCrCb / YVU planar
            // convert to flex YUV
            ycbcr.y = bits;
            ycbcr.cr = (uint8_t *)bits + srcStride * srcVStride;
            ycbcr.cb = (uint8_t *)ycbcr.cr + (srcStride >> 1) * (srcVStride >> 1);
            ycbcr.chroma_step = 1;
            ycbcr.cstride = srcVStride >> 1;
            ycbcr.ystride = srcVStride;
            ConvertFlexYUVToPlanar(dst, dstStride, dstVStride, &ycbcr, width, height);
            break;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:  // YCrCb / YVU semiplanar, NV21
            // convert to flex YUV
            ycbcr.y = bits;
            ycbcr.cr = (uint8_t *)bits + srcStride * srcVStride;
            ycbcr.cb = (uint8_t *)ycbcr.cr + 1;
            ycbcr.chroma_step = 2;
            ycbcr.cstride = srcVStride;
            ycbcr.ystride = srcVStride;
            ConvertFlexYUVToPlanar(dst, dstStride, dstVStride, &ycbcr, width, height);
            break;
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
            ConvertFlexYUVToPlanar(dst, dstStride, dstVStride, &ycbcr, width, height);
            break;
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            ConvertRGB32ToPlanar(
                    dst, dstStride, dstVStride,
                    (const uint8_t *)bits, width, height, srcStride,
                    format == HAL_PIXEL_FORMAT_BGRA_8888);
            break;
        default:
            ALOGE("Unsupported pixel format %#x", format);
            dst = NULL;
            break;
    }

    if (grmodule->unlock(grmodule, handle) != OK) {
        ALOGE("Unable to unlock image buffer %p for access", handle);
    }

    return dst;
}

OMX_ERRORTYPE SoftVideoEncoderOMXComponent::getExtensionIndex(
        const char *name, OMX_INDEXTYPE *index) {
    if (!strcmp(name, "OMX.google.android.index.storeMetaDataInBuffers") ||
        !strcmp(name, "OMX.google.android.index.storeGraphicBufferInMetaData")) {
        *(int32_t*)index = kStoreMetaDataExtensionIndex;
        return OMX_ErrorNone;
    }
    return SimpleSoftOMXComponent::getExtensionIndex(name, index);
}

}  // namespace android
