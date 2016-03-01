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

//#define LOG_NDEBUG 0
#define LOG_TAG "OMXUtils"

#include <string.h>

#include <media/hardware/HardwareAPI.h>
#include <media/stagefright/MediaErrors.h>
#include "OMXUtils.h"

namespace android {

status_t StatusFromOMXError(OMX_ERRORTYPE err) {
    switch (err) {
        case OMX_ErrorNone:
            return OK;
        case OMX_ErrorUnsupportedSetting:
        case OMX_ErrorUnsupportedIndex:
            return ERROR_UNSUPPORTED; // this is a media specific error
        case OMX_ErrorInsufficientResources:
            return NO_MEMORY;
        case OMX_ErrorInvalidComponentName:
        case OMX_ErrorComponentNotFound:
            return NAME_NOT_FOUND;
        default:
            return UNKNOWN_ERROR;
    }
}

/**************************************************************************************************/

DescribeColorFormatParams::DescribeColorFormatParams(const DescribeColorFormat2Params &params) {
    InitOMXParams(this);

    eColorFormat = params.eColorFormat;
    nFrameWidth = params.nFrameWidth;
    nFrameHeight = params.nFrameHeight;
    nStride = params.nStride;
    nSliceHeight = params.nSliceHeight;
    bUsingNativeBuffers = params.bUsingNativeBuffers;
    // we don't copy media images as this conversion is only used pre-query
};

void DescribeColorFormat2Params::initFromV1(const DescribeColorFormatParams &params) {
    InitOMXParams(this);

    eColorFormat = params.eColorFormat;
    nFrameWidth = params.nFrameWidth;
    nFrameHeight = params.nFrameHeight;
    nStride = params.nStride;
    nSliceHeight = params.nSliceHeight;
    bUsingNativeBuffers = params.bUsingNativeBuffers;
    sMediaImage.initFromV1(params.sMediaImage);
};

void MediaImage2::initFromV1(const MediaImage &image) {
    memset(this, 0, sizeof(*this));

    if (image.mType != MediaImage::MEDIA_IMAGE_TYPE_YUV) {
        mType = MediaImage2::MEDIA_IMAGE_TYPE_UNKNOWN;
        return;
    }

    for (size_t ix = 0; ix < image.mNumPlanes; ++ix) {
        if (image.mPlane[ix].mHorizSubsampling > INT32_MAX
                || image.mPlane[ix].mVertSubsampling > INT32_MAX) {
            mType = MediaImage2::MEDIA_IMAGE_TYPE_UNKNOWN;
            return;
        }
    }

    mType = (MediaImage2::Type)image.mType;
    mNumPlanes = image.mNumPlanes;
    mWidth = image.mWidth;
    mHeight = image.mHeight;
    mBitDepth = image.mBitDepth;
    mBitDepthAllocated = 8;
    for (size_t ix = 0; ix < image.mNumPlanes; ++ix) {
        mPlane[ix].mOffset = image.mPlane[ix].mOffset;
        mPlane[ix].mColInc = image.mPlane[ix].mColInc;
        mPlane[ix].mRowInc = image.mPlane[ix].mRowInc;
        mPlane[ix].mHorizSubsampling = (int32_t)image.mPlane[ix].mHorizSubsampling;
        mPlane[ix].mVertSubsampling = (int32_t)image.mPlane[ix].mVertSubsampling;
    }
}

/**************************************************************************************************/

}  // namespace android

