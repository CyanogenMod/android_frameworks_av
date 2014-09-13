/*
 * Copyright (C) 2013 The Android Open Source Project
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
#define LOG_TAG "SoftVideoDecoderOMXComponent"
#include <utils/Log.h>

#include "include/SoftVideoDecoderOMXComponent.h"

#include <media/hardware/HardwareAPI.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaDefs.h>

namespace android {

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

SoftVideoDecoderOMXComponent::SoftVideoDecoderOMXComponent(
        const char *name,
        const char *componentRole,
        OMX_VIDEO_CODINGTYPE codingType,
        const CodecProfileLevel *profileLevels,
        size_t numProfileLevels,
        int32_t width,
        int32_t height,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
        : SimpleSoftOMXComponent(name, callbacks, appData, component),
        mIsAdaptive(false),
        mAdaptiveMaxWidth(0),
        mAdaptiveMaxHeight(0),
        mWidth(width),
        mHeight(height),
        mCropLeft(0),
        mCropTop(0),
        mCropWidth(width),
        mCropHeight(height),
        mOutputPortSettingsChange(NONE),
        mComponentRole(componentRole),
        mCodingType(codingType),
        mProfileLevels(profileLevels),
        mNumProfileLevels(numProfileLevels) {
}

void SoftVideoDecoderOMXComponent::initPorts(
        OMX_U32 numInputBuffers,
        OMX_U32 inputBufferSize,
        OMX_U32 numOutputBuffers,
        const char *mimeType) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    def.nPortIndex = kInputPortIndex;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = numInputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = inputBufferSize;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 1;

    def.format.video.cMIMEType = const_cast<char *>(mimeType);
    def.format.video.pNativeRender = NULL;
    /* size is initialized in updatePortDefinitions() */
    def.format.video.nBitrate = 0;
    def.format.video.xFramerate = 0;
    def.format.video.bFlagErrorConcealment = OMX_FALSE;
    def.format.video.eCompressionFormat = mCodingType;
    def.format.video.eColorFormat = OMX_COLOR_FormatUnused;
    def.format.video.pNativeWindow = NULL;

    addPort(def);

    def.nPortIndex = kOutputPortIndex;
    def.eDir = OMX_DirOutput;
    def.nBufferCountMin = numOutputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 2;

    def.format.video.cMIMEType = const_cast<char *>("video/raw");
    def.format.video.pNativeRender = NULL;
    /* size is initialized in updatePortDefinitions() */
    def.format.video.nBitrate = 0;
    def.format.video.xFramerate = 0;
    def.format.video.bFlagErrorConcealment = OMX_FALSE;
    def.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
    def.format.video.eColorFormat = OMX_COLOR_FormatYUV420Planar;
    def.format.video.pNativeWindow = NULL;

    addPort(def);

    updatePortDefinitions();
}

void SoftVideoDecoderOMXComponent::updatePortDefinitions(bool updateCrop) {
    OMX_PARAM_PORTDEFINITIONTYPE *def = &editPortInfo(kInputPortIndex)->mDef;
    def->format.video.nFrameWidth = mWidth;
    def->format.video.nFrameHeight = mHeight;
    def->format.video.nStride = def->format.video.nFrameWidth;
    def->format.video.nSliceHeight = def->format.video.nFrameHeight;

    def->nBufferSize = def->format.video.nFrameWidth * def->format.video.nFrameHeight * 3 / 2;

    def = &editPortInfo(kOutputPortIndex)->mDef;
    def->format.video.nFrameWidth = mIsAdaptive ? mAdaptiveMaxWidth : mWidth;
    def->format.video.nFrameHeight = mIsAdaptive ? mAdaptiveMaxHeight : mHeight;
    def->format.video.nStride = def->format.video.nFrameWidth;
    def->format.video.nSliceHeight = def->format.video.nFrameHeight;

    def->nBufferSize =
            (def->format.video.nFrameWidth *
             def->format.video.nFrameHeight * 3) / 2;

    if (updateCrop) {
        mCropLeft = 0;
        mCropTop = 0;
        mCropWidth = mWidth;
        mCropHeight = mHeight;
    }
}

void SoftVideoDecoderOMXComponent::handlePortSettingsChange(
        bool *portWillReset, uint32_t width, uint32_t height, bool cropChanged, bool fakeStride) {
    *portWillReset = false;
    bool sizeChanged = (width != mWidth || height != mHeight);

    if (sizeChanged || cropChanged) {
        mWidth = width;
        mHeight = height;

        bool updateCrop = !cropChanged;
        if ((sizeChanged && !mIsAdaptive)
            || width > mAdaptiveMaxWidth
            || height > mAdaptiveMaxHeight) {
            if (mIsAdaptive) {
                if (width > mAdaptiveMaxWidth) {
                    mAdaptiveMaxWidth = width;
                }
                if (height > mAdaptiveMaxHeight) {
                    mAdaptiveMaxHeight = height;
                }
            }
            updatePortDefinitions(updateCrop);
            notify(OMX_EventPortSettingsChanged, kOutputPortIndex, 0, NULL);
            mOutputPortSettingsChange = AWAITING_DISABLED;
            *portWillReset = true;
        } else {
            updatePortDefinitions(updateCrop);

            if (fakeStride) {
                // MAJOR HACK that is not pretty, it's just to fool the renderer to read the correct
                // data.
                // Some software decoders (e.g. SoftMPEG4) fill decoded frame directly to output
                // buffer without considering the output buffer stride and slice height. So this is
                // used to signal how the buffer is arranged.  The alternative is to re-arrange the
                // output buffer in SoftMPEG4, but that results in memcopies.
                OMX_PARAM_PORTDEFINITIONTYPE *def = &editPortInfo(kOutputPortIndex)->mDef;
                def->format.video.nStride = mWidth;
                def->format.video.nSliceHeight = mHeight;
            }

            notify(OMX_EventPortSettingsChanged, kOutputPortIndex,
                   OMX_IndexConfigCommonOutputCrop, NULL);
        }
    }
}

void SoftVideoDecoderOMXComponent::copyYV12FrameToOutputBuffer(
        uint8_t *dst, const uint8_t *srcY, const uint8_t *srcU, const uint8_t *srcV,
        size_t srcYStride, size_t srcUStride, size_t srcVStride) {
    size_t dstYStride = mIsAdaptive ? mAdaptiveMaxWidth : mWidth;
    size_t dstUVStride = dstYStride / 2;
    size_t dstHeight = mIsAdaptive ? mAdaptiveMaxHeight : mHeight;

    for (size_t i = 0; i < dstHeight; ++i) {
        if (i < mHeight) {
            memcpy(dst, srcY, mWidth);
            srcY += srcYStride;
        }
        dst += dstYStride;
    }

    for (size_t i = 0; i < dstHeight / 2; ++i) {
        if (i < mHeight / 2) {
            memcpy(dst, srcU, mWidth / 2);
            srcU += srcUStride;
        }
        dst += dstUVStride;
    }

    for (size_t i = 0; i < dstHeight / 2; ++i) {
        if (i < mHeight / 2) {
            memcpy(dst, srcV, mWidth / 2);
            srcV += srcVStride;
        }
        dst += dstUVStride;
    }
}

OMX_ERRORTYPE SoftVideoDecoderOMXComponent::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamVideoPortFormat:
        {
            OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
                (OMX_VIDEO_PARAM_PORTFORMATTYPE *)params;

            if (formatParams->nPortIndex > kMaxPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (formatParams->nIndex != 0) {
                return OMX_ErrorNoMore;
            }

            if (formatParams->nPortIndex == kInputPortIndex) {
                formatParams->eCompressionFormat = mCodingType;
                formatParams->eColorFormat = OMX_COLOR_FormatUnused;
                formatParams->xFramerate = 0;
            } else {
                CHECK_EQ(formatParams->nPortIndex, 1u);

                formatParams->eCompressionFormat = OMX_VIDEO_CodingUnused;
                formatParams->eColorFormat = OMX_COLOR_FormatYUV420Planar;
                formatParams->xFramerate = 0;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoProfileLevelQuerySupported:
        {
            OMX_VIDEO_PARAM_PROFILELEVELTYPE *profileLevel =
                  (OMX_VIDEO_PARAM_PROFILELEVELTYPE *) params;

            if (profileLevel->nPortIndex != kInputPortIndex) {
                ALOGE("Invalid port index: %" PRIu32, profileLevel->nPortIndex);
                return OMX_ErrorUnsupportedIndex;
            }

            if (profileLevel->nProfileIndex >= mNumProfileLevels) {
                return OMX_ErrorNoMore;
            }

            profileLevel->eProfile = mProfileLevels[profileLevel->nProfileIndex].mProfile;
            profileLevel->eLevel   = mProfileLevels[profileLevel->nProfileIndex].mLevel;
            return OMX_ErrorNone;
        }

        default:
            return SimpleSoftOMXComponent::internalGetParameter(index, params);
    }
}

OMX_ERRORTYPE SoftVideoDecoderOMXComponent::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    // Include extension index OMX_INDEXEXTTYPE.
    const int32_t indexFull = index;

    switch (indexFull) {
        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams =
                (const OMX_PARAM_COMPONENTROLETYPE *)params;

            if (strncmp((const char *)roleParams->cRole,
                        mComponentRole,
                        OMX_MAX_STRINGNAME_SIZE - 1)) {
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoPortFormat:
        {
            OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
                (OMX_VIDEO_PARAM_PORTFORMATTYPE *)params;

            if (formatParams->nPortIndex > kMaxPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (formatParams->nIndex != 0) {
                return OMX_ErrorNoMore;
            }

            return OMX_ErrorNone;
        }

        case kPrepareForAdaptivePlaybackIndex:
        {
            const PrepareForAdaptivePlaybackParams* adaptivePlaybackParams =
                    (const PrepareForAdaptivePlaybackParams *)params;
            mIsAdaptive = adaptivePlaybackParams->bEnable;
            if (mIsAdaptive) {
                mAdaptiveMaxWidth = adaptivePlaybackParams->nMaxFrameWidth;
                mAdaptiveMaxHeight = adaptivePlaybackParams->nMaxFrameHeight;
                mWidth = mAdaptiveMaxWidth;
                mHeight = mAdaptiveMaxHeight;
            } else {
                mAdaptiveMaxWidth = 0;
                mAdaptiveMaxHeight = 0;
            }
            updatePortDefinitions();
            return OMX_ErrorNone;
        }

        default:
            return SimpleSoftOMXComponent::internalSetParameter(index, params);
    }
}

OMX_ERRORTYPE SoftVideoDecoderOMXComponent::getConfig(
        OMX_INDEXTYPE index, OMX_PTR params) {
    switch (index) {
        case OMX_IndexConfigCommonOutputCrop:
        {
            OMX_CONFIG_RECTTYPE *rectParams = (OMX_CONFIG_RECTTYPE *)params;

            if (rectParams->nPortIndex != kOutputPortIndex) {
                return OMX_ErrorUndefined;
            }

            rectParams->nLeft = mCropLeft;
            rectParams->nTop = mCropTop;
            rectParams->nWidth = mCropWidth;
            rectParams->nHeight = mCropHeight;

            return OMX_ErrorNone;
        }

        default:
            return OMX_ErrorUnsupportedIndex;
    }
}

OMX_ERRORTYPE SoftVideoDecoderOMXComponent::getExtensionIndex(
        const char *name, OMX_INDEXTYPE *index) {
    if (!strcmp(name, "OMX.google.android.index.prepareForAdaptivePlayback")) {
        *(int32_t*)index = kPrepareForAdaptivePlaybackIndex;
        return OMX_ErrorNone;
    }

    return SimpleSoftOMXComponent::getExtensionIndex(name, index);
}

void SoftVideoDecoderOMXComponent::onReset() {
    mOutputPortSettingsChange = NONE;
}

void SoftVideoDecoderOMXComponent::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
    if (portIndex != kOutputPortIndex) {
        return;
    }

    switch (mOutputPortSettingsChange) {
        case NONE:
            break;

        case AWAITING_DISABLED:
        {
            CHECK(!enabled);
            mOutputPortSettingsChange = AWAITING_ENABLED;
            break;
        }

        default:
        {
            CHECK_EQ((int)mOutputPortSettingsChange, (int)AWAITING_ENABLED);
            CHECK(enabled);
            mOutputPortSettingsChange = NONE;
            break;
        }
    }
}

}  // namespace android
