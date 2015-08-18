/*
 * Copyright (C) 2012 The Android Open Source Project
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
#define LOG_TAG "SoftAVCEncoder"
#include <utils/Log.h>
#include <utils/misc.h>

#include "avcenc_api.h"
#include "avcenc_int.h"
#include "OMX_Video.h"

#include <HardwareAPI.h>
#include <MetadataBufferType.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <ui/Rect.h>
#include <ui/GraphicBufferMapper.h>

#include "SoftAVCEncoder.h"

#if LOG_NDEBUG
#define UNUSED_UNLESS_VERBOSE(x) (void)(x)
#else
#define UNUSED_UNLESS_VERBOSE(x)
#endif

namespace android {

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

static const CodecProfileLevel kProfileLevels[] = {
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel2  },
};

typedef struct LevelConversion {
    OMX_U32 omxLevel;
    AVCLevel avcLevel;
    uint32_t maxMacroBlocks;
} LevelConcersion;

static LevelConversion ConversionTable[] = {
    { OMX_VIDEO_AVCLevel1,  AVC_LEVEL1_B, 99 },
    { OMX_VIDEO_AVCLevel1b, AVC_LEVEL1,   99 },
    { OMX_VIDEO_AVCLevel11, AVC_LEVEL1_1, 396 },
    { OMX_VIDEO_AVCLevel12, AVC_LEVEL1_2, 396 },
    { OMX_VIDEO_AVCLevel13, AVC_LEVEL1_3, 396 },
    { OMX_VIDEO_AVCLevel2,  AVC_LEVEL2,   396 },
#if 0
    // encoding speed is very poor if video resolution
    // is higher than CIF or if level is higher than 2
    { OMX_VIDEO_AVCLevel21, AVC_LEVEL2_1, 792 },
    { OMX_VIDEO_AVCLevel22, AVC_LEVEL2_2, 1620 },
    { OMX_VIDEO_AVCLevel3,  AVC_LEVEL3,   1620 },
    { OMX_VIDEO_AVCLevel31, AVC_LEVEL3_1, 3600 },
    { OMX_VIDEO_AVCLevel32, AVC_LEVEL3_2, 5120 },
    { OMX_VIDEO_AVCLevel4,  AVC_LEVEL4,   8192 },
    { OMX_VIDEO_AVCLevel41, AVC_LEVEL4_1, 8192 },
    { OMX_VIDEO_AVCLevel42, AVC_LEVEL4_2, 8704 },
    { OMX_VIDEO_AVCLevel5,  AVC_LEVEL5,   22080 },
    { OMX_VIDEO_AVCLevel51, AVC_LEVEL5_1, 36864 },
#endif
};

static status_t ConvertOmxAvcLevelToAvcSpecLevel(
        OMX_U32 omxLevel, AVCLevel *avcLevel) {
    for (size_t i = 0, n = sizeof(ConversionTable)/sizeof(ConversionTable[0]);
        i < n; ++i) {
        if (omxLevel == ConversionTable[i].omxLevel) {
            *avcLevel = ConversionTable[i].avcLevel;
            return OK;
        }
    }

    ALOGE("ConvertOmxAvcLevelToAvcSpecLevel: %d level not supported",
            (int32_t)omxLevel);

    return BAD_VALUE;
}

static status_t ConvertAvcSpecLevelToOmxAvcLevel(
    AVCLevel avcLevel, OMX_U32 *omxLevel) {
    for (size_t i = 0, n = sizeof(ConversionTable)/sizeof(ConversionTable[0]);
        i < n; ++i) {
        if (avcLevel == ConversionTable[i].avcLevel) {
            *omxLevel = ConversionTable[i].omxLevel;
            return OK;
        }
    }

    ALOGE("ConvertAvcSpecLevelToOmxAvcLevel: %d level not supported",
            (int32_t) avcLevel);

    return BAD_VALUE;
}

static void* MallocWrapper(
        void * /* userData */, int32_t size, int32_t /* attrs */) {
    void *ptr = malloc(size);
    if (ptr)
        memset(ptr, 0, size);
    return ptr;
}

static void FreeWrapper(void * /* userData */, void* ptr) {
    free(ptr);
}

static int32_t DpbAllocWrapper(void *userData,
        unsigned int sizeInMbs, unsigned int numBuffers) {
    SoftAVCEncoder *encoder = static_cast<SoftAVCEncoder *>(userData);
    CHECK(encoder != NULL);
    return encoder->allocOutputBuffers(sizeInMbs, numBuffers);
}

static int32_t BindFrameWrapper(
        void *userData, int32_t index, uint8_t **yuv) {
    SoftAVCEncoder *encoder = static_cast<SoftAVCEncoder *>(userData);
    CHECK(encoder != NULL);
    return encoder->bindOutputBuffer(index, yuv);
}

static void UnbindFrameWrapper(void *userData, int32_t index) {
    SoftAVCEncoder *encoder = static_cast<SoftAVCEncoder *>(userData);
    CHECK(encoder != NULL);
    return encoder->unbindOutputBuffer(index);
}

SoftAVCEncoder::SoftAVCEncoder(
            const char *name,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component)
    : SoftVideoEncoderOMXComponent(
            name, "video_encoder.avc", OMX_VIDEO_CodingAVC,
            kProfileLevels, NELEM(kProfileLevels),
            176 /* width */, 144 /* height */,
            callbacks, appData, component),
      mIDRFrameRefreshIntervalInSec(1),
      mAVCEncProfile(AVC_BASELINE),
      mAVCEncLevel(AVC_LEVEL2),
      mNumInputFrames(-1),
      mPrevTimestampUs(-1),
      mStarted(false),
      mSawInputEOS(false),
      mSignalledError(false),
      mHandle(new tagAVCHandle),
      mEncParams(new tagAVCEncParam),
      mInputFrameData(NULL),
      mSliceGroup(NULL) {

    const size_t kOutputBufferSize =
        320 * ConversionTable[NELEM(ConversionTable) - 1].maxMacroBlocks;

    initPorts(
            kNumBuffers, kNumBuffers, kOutputBufferSize,
            MEDIA_MIMETYPE_VIDEO_AVC, 2 /* minCompressionRatio */);

    ALOGI("Construct SoftAVCEncoder");
}

SoftAVCEncoder::~SoftAVCEncoder() {
    ALOGV("Destruct SoftAVCEncoder");
    releaseEncoder();
    List<BufferInfo *> &outQueue = getPortQueue(1);
    List<BufferInfo *> &inQueue = getPortQueue(0);
    CHECK(outQueue.empty());
    CHECK(inQueue.empty());
}

OMX_ERRORTYPE SoftAVCEncoder::initEncParams() {
    CHECK(mHandle != NULL);
    memset(mHandle, 0, sizeof(tagAVCHandle));
    mHandle->AVCObject = NULL;
    mHandle->userData = this;
    mHandle->CBAVC_DPBAlloc = DpbAllocWrapper;
    mHandle->CBAVC_FrameBind = BindFrameWrapper;
    mHandle->CBAVC_FrameUnbind = UnbindFrameWrapper;
    mHandle->CBAVC_Malloc = MallocWrapper;
    mHandle->CBAVC_Free = FreeWrapper;

    CHECK(mEncParams != NULL);
    memset(mEncParams, 0, sizeof(*mEncParams));
    mEncParams->rate_control = AVC_ON;
    mEncParams->initQP = 0;
    mEncParams->init_CBP_removal_delay = 1600;

    mEncParams->intramb_refresh = 0;
    mEncParams->auto_scd = AVC_ON;
    mEncParams->out_of_band_param_set = AVC_ON;
    mEncParams->poc_type = 2;
    mEncParams->log2_max_poc_lsb_minus_4 = 12;
    mEncParams->delta_poc_zero_flag = 0;
    mEncParams->offset_poc_non_ref = 0;
    mEncParams->offset_top_bottom = 0;
    mEncParams->num_ref_in_cycle = 0;
    mEncParams->offset_poc_ref = NULL;

    mEncParams->num_ref_frame = 1;
    mEncParams->num_slice_group = 1;
    mEncParams->fmo_type = 0;

    mEncParams->db_filter = AVC_ON;
    mEncParams->disable_db_idc = 0;

    mEncParams->alpha_offset = 0;
    mEncParams->beta_offset = 0;
    mEncParams->constrained_intra_pred = AVC_OFF;

    mEncParams->data_par = AVC_OFF;
    mEncParams->fullsearch = AVC_OFF;
    mEncParams->search_range = 16;
    mEncParams->sub_pel = AVC_OFF;
    mEncParams->submb_pred = AVC_OFF;
    mEncParams->rdopt_mode = AVC_OFF;
    mEncParams->bidir_pred = AVC_OFF;

    mEncParams->use_overrun_buffer = AVC_OFF;

    if (mColorFormat != OMX_COLOR_FormatYUV420Planar || mInputDataIsMeta) {
        // Color conversion is needed.
        free(mInputFrameData);
        if (((uint64_t)mWidth * mHeight) > ((uint64_t)INT32_MAX / 3)) {
            ALOGE("Buffer size is too big.");
            return OMX_ErrorUndefined;
        }
        mInputFrameData =
            (uint8_t *) malloc((mWidth * mHeight * 3 ) >> 1);
        CHECK(mInputFrameData != NULL);
    }

    // PV's AVC encoder requires the video dimension of multiple
    if (mWidth % 16 != 0 || mHeight % 16 != 0) {
        ALOGE("Video frame size %dx%d must be a multiple of 16",
            mWidth, mHeight);
        return OMX_ErrorBadParameter;
    }

    mEncParams->width = mWidth;
    mEncParams->height = mHeight;
    mEncParams->bitrate = mBitrate;
    mEncParams->frame_rate = (1000 * mFramerate) >> 16;  // In frames/ms!, mFramerate is in Q16
    mEncParams->CPB_size = (uint32_t) (mBitrate >> 1);

    int32_t nMacroBlocks = divUp(mWidth, 16) * divUp(mHeight, 16);
    CHECK(mSliceGroup == NULL);
    if ((size_t)nMacroBlocks > SIZE_MAX / sizeof(uint32_t)) {
        ALOGE("requested memory size is too big.");
        return OMX_ErrorUndefined;
    }
    mSliceGroup = (uint32_t *) malloc(sizeof(uint32_t) * nMacroBlocks);
    CHECK(mSliceGroup != NULL);
    for (int ii = 0, idx = 0; ii < nMacroBlocks; ++ii) {
        mSliceGroup[ii] = idx++;
        if (idx >= mEncParams->num_slice_group) {
            idx = 0;
        }
    }
    mEncParams->slice_group = mSliceGroup;

    // Set IDR frame refresh interval
    if (mIDRFrameRefreshIntervalInSec < 0) {
        mEncParams->idr_period = -1;
    } else if (mIDRFrameRefreshIntervalInSec == 0) {
        mEncParams->idr_period = 1;  // All I frames
    } else {
        mEncParams->idr_period =
            (mIDRFrameRefreshIntervalInSec * mFramerate) >> 16; // mFramerate is in Q16
    }

    // Set profile and level
    mEncParams->profile = mAVCEncProfile;
    mEncParams->level = mAVCEncLevel;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE SoftAVCEncoder::initEncoder() {
    CHECK(!mStarted);

    OMX_ERRORTYPE errType = OMX_ErrorNone;
    if (OMX_ErrorNone != (errType = initEncParams())) {
        ALOGE("Failed to initialized encoder params");
        mSignalledError = true;
        notify(OMX_EventError, OMX_ErrorUndefined, 0, 0);
        return errType;
    }

    AVCEnc_Status err;
    err = PVAVCEncInitialize(mHandle, mEncParams, NULL, NULL);
    if (err != AVCENC_SUCCESS) {
        ALOGE("Failed to initialize the encoder: %d", err);
        mSignalledError = true;
        notify(OMX_EventError, OMX_ErrorUndefined, 0, 0);
        return OMX_ErrorUndefined;
    }

    mNumInputFrames = -2;  // 1st two buffers contain SPS and PPS
    mSpsPpsHeaderReceived = false;
    mReadyForNextFrame = true;
    mIsIDRFrame = false;
    mStarted = true;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE SoftAVCEncoder::releaseEncoder() {
    if (!mStarted) {
        return OMX_ErrorNone;
    }

    PVAVCCleanUpEncoder(mHandle);
    releaseOutputBuffers();

    free(mInputFrameData);
    mInputFrameData = NULL;

    free(mSliceGroup);
    mSliceGroup = NULL;

    delete mEncParams;
    mEncParams = NULL;

    delete mHandle;
    mHandle = NULL;

    mStarted = false;

    return OMX_ErrorNone;
}

void SoftAVCEncoder::releaseOutputBuffers() {
    for (size_t i = 0; i < mOutputBuffers.size(); ++i) {
        MediaBuffer *buffer = mOutputBuffers.editItemAt(i);
        buffer->setObserver(NULL);
        buffer->release();
    }
    mOutputBuffers.clear();
}

OMX_ERRORTYPE SoftAVCEncoder::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamVideoBitrate:
        {
            OMX_VIDEO_PARAM_BITRATETYPE *bitRate =
                (OMX_VIDEO_PARAM_BITRATETYPE *) params;

            if (bitRate->nPortIndex != 1) {
                return OMX_ErrorUndefined;
            }

            bitRate->eControlRate = OMX_Video_ControlRateVariable;
            bitRate->nTargetBitrate = mBitrate;
            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoAvc:
        {
            OMX_VIDEO_PARAM_AVCTYPE *avcParams =
                (OMX_VIDEO_PARAM_AVCTYPE *)params;

            if (avcParams->nPortIndex != 1) {
                return OMX_ErrorUndefined;
            }

            avcParams->eProfile = OMX_VIDEO_AVCProfileBaseline;
            OMX_U32 omxLevel = AVC_LEVEL2;
            if (OMX_ErrorNone !=
                ConvertAvcSpecLevelToOmxAvcLevel(mAVCEncLevel, &omxLevel)) {
                return OMX_ErrorUndefined;
            }

            avcParams->eLevel = (OMX_VIDEO_AVCLEVELTYPE) omxLevel;
            avcParams->nRefFrames = 1;
            avcParams->nBFrames = 0;
            avcParams->bUseHadamard = OMX_TRUE;
            avcParams->nAllowedPictureTypes =
                    (OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP);
            avcParams->nRefIdx10ActiveMinus1 = 0;
            avcParams->nRefIdx11ActiveMinus1 = 0;
            avcParams->bWeightedPPrediction = OMX_FALSE;
            avcParams->bEntropyCodingCABAC = OMX_FALSE;
            avcParams->bconstIpred = OMX_FALSE;
            avcParams->bDirect8x8Inference = OMX_FALSE;
            avcParams->bDirectSpatialTemporal = OMX_FALSE;
            avcParams->nCabacInitIdc = 0;
            return OMX_ErrorNone;
        }

        default:
            return SoftVideoEncoderOMXComponent::internalGetParameter(index, params);
    }
}

OMX_ERRORTYPE SoftAVCEncoder::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    int32_t indexFull = index;

    switch (indexFull) {
        case OMX_IndexParamVideoBitrate:
        {
            OMX_VIDEO_PARAM_BITRATETYPE *bitRate =
                (OMX_VIDEO_PARAM_BITRATETYPE *) params;

            if (bitRate->nPortIndex != 1 ||
                bitRate->eControlRate != OMX_Video_ControlRateVariable) {
                return OMX_ErrorUndefined;
            }

            mBitrate = bitRate->nTargetBitrate;
            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoAvc:
        {
            OMX_VIDEO_PARAM_AVCTYPE *avcType =
                (OMX_VIDEO_PARAM_AVCTYPE *)params;

            if (avcType->nPortIndex != 1) {
                return OMX_ErrorUndefined;
            }

            // PV's AVC encoder only supports baseline profile
            if (avcType->eProfile != OMX_VIDEO_AVCProfileBaseline ||
                avcType->nRefFrames != 1 ||
                avcType->nBFrames != 0 ||
                avcType->bUseHadamard != OMX_TRUE ||
                (avcType->nAllowedPictureTypes & OMX_VIDEO_PictureTypeB) != 0 ||
                avcType->nRefIdx10ActiveMinus1 != 0 ||
                avcType->nRefIdx11ActiveMinus1 != 0 ||
                avcType->bWeightedPPrediction != OMX_FALSE ||
                avcType->bEntropyCodingCABAC != OMX_FALSE ||
                avcType->bconstIpred != OMX_FALSE ||
                avcType->bDirect8x8Inference != OMX_FALSE ||
                avcType->bDirectSpatialTemporal != OMX_FALSE ||
                avcType->nCabacInitIdc != 0) {
                return OMX_ErrorUndefined;
            }

            if (OK != ConvertOmxAvcLevelToAvcSpecLevel(avcType->eLevel, &mAVCEncLevel)) {
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        default:
            return SoftVideoEncoderOMXComponent::internalSetParameter(index, params);
    }
}

void SoftAVCEncoder::onQueueFilled(OMX_U32 /* portIndex */) {
    if (mSignalledError || mSawInputEOS) {
        return;
    }

    if (!mStarted) {
        if (OMX_ErrorNone != initEncoder()) {
            return;
        }
    }

    List<BufferInfo *> &inQueue = getPortQueue(0);
    List<BufferInfo *> &outQueue = getPortQueue(1);

    while (!mSawInputEOS && !inQueue.empty() && !outQueue.empty()) {
        BufferInfo *inInfo = *inQueue.begin();
        OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;
        BufferInfo *outInfo = *outQueue.begin();
        OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

        outHeader->nTimeStamp = 0;
        outHeader->nFlags = 0;
        outHeader->nOffset = 0;
        outHeader->nFilledLen = 0;
        outHeader->nOffset = 0;

        uint8_t *outPtr = (uint8_t *) outHeader->pBuffer;
        uint32_t dataLength = outHeader->nAllocLen;

        if (!mSpsPpsHeaderReceived && mNumInputFrames < 0) {
            // 4 bytes are reserved for holding the start code 0x00000001
            // of the sequence parameter set at the beginning.
            outPtr += 4;
            dataLength -= 4;
        }

        int32_t type;
        AVCEnc_Status encoderStatus = AVCENC_SUCCESS;

        // Combine SPS and PPS and place them in the very first output buffer
        // SPS and PPS are separated by start code 0x00000001
        // Assume that we have exactly one SPS and exactly one PPS.
        while (!mSpsPpsHeaderReceived && mNumInputFrames <= 0) {
            encoderStatus = PVAVCEncodeNAL(mHandle, outPtr, &dataLength, &type);
            if (encoderStatus == AVCENC_WRONG_STATE) {
                mSpsPpsHeaderReceived = true;
                CHECK_EQ(0, mNumInputFrames);  // 1st video frame is 0
                outHeader->nFlags = OMX_BUFFERFLAG_CODECCONFIG;
                outQueue.erase(outQueue.begin());
                outInfo->mOwnedByUs = false;
                notifyFillBufferDone(outHeader);
                return;
            } else {
                switch (type) {
                    case AVC_NALTYPE_SPS:
                        ++mNumInputFrames;
                        memcpy((uint8_t *)outHeader->pBuffer, "\x00\x00\x00\x01", 4);
                        outHeader->nFilledLen = 4 + dataLength;
                        outPtr += (dataLength + 4);  // 4 bytes for next start code
                        dataLength = outHeader->nAllocLen - outHeader->nFilledLen;
                        break;
                    default:
                        CHECK_EQ(AVC_NALTYPE_PPS, type);
                        ++mNumInputFrames;
                        memcpy((uint8_t *) outHeader->pBuffer + outHeader->nFilledLen,
                                "\x00\x00\x00\x01", 4);
                        outHeader->nFilledLen += (dataLength + 4);
                        outPtr += (dataLength + 4);
                        break;
                }
            }
        }

        // Get next input video frame
        if (mReadyForNextFrame) {
            // Save the input buffer info so that it can be
            // passed to an output buffer
            InputBufferInfo info;
            info.mTimeUs = inHeader->nTimeStamp;
            info.mFlags = inHeader->nFlags;
            mInputBufferInfoVec.push(info);
            mPrevTimestampUs = inHeader->nTimeStamp;

            if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
                mSawInputEOS = true;
            }

            if (inHeader->nFilledLen > 0) {
                AVCFrameIO videoInput;
                memset(&videoInput, 0, sizeof(videoInput));
                videoInput.height = align(mHeight, 16);
                videoInput.pitch = align(mWidth, 16);
                videoInput.coding_timestamp = (inHeader->nTimeStamp + 500) / 1000;  // in ms
                const uint8_t *inputData = NULL;
                if (mInputDataIsMeta) {
                    if (inHeader->nFilledLen != 8) {
                        ALOGE("MetaData buffer is wrong size! "
                                "(got %u bytes, expected 8)", inHeader->nFilledLen);
                        mSignalledError = true;
                        notify(OMX_EventError, OMX_ErrorUndefined, 0, 0);
                        return;
                    }
                    inputData =
                        extractGraphicBuffer(
                                mInputFrameData, (mWidth * mHeight * 3) >> 1,
                                inHeader->pBuffer + inHeader->nOffset, inHeader->nFilledLen,
                                mWidth, mHeight);
                    if (inputData == NULL) {
                        ALOGE("Unable to extract gralloc buffer in metadata mode");
                        mSignalledError = true;
                        notify(OMX_EventError, OMX_ErrorUndefined, 0, 0);
                        return;
                    }
                    // TODO: Verify/convert pixel format enum
                } else {
                    inputData = (const uint8_t *)inHeader->pBuffer + inHeader->nOffset;
                    if (mColorFormat != OMX_COLOR_FormatYUV420Planar) {
                        ConvertYUV420SemiPlanarToYUV420Planar(
                            inputData, mInputFrameData, mWidth, mHeight);
                        inputData = mInputFrameData;
                    }
                }

                CHECK(inputData != NULL);
                videoInput.YCbCr[0] = (uint8_t *)inputData;
                videoInput.YCbCr[1] = videoInput.YCbCr[0] + videoInput.height * videoInput.pitch;
                videoInput.YCbCr[2] = videoInput.YCbCr[1] +
                    ((videoInput.height * videoInput.pitch) >> 2);
                videoInput.disp_order = mNumInputFrames;

                encoderStatus = PVAVCEncSetInput(mHandle, &videoInput);
                if (encoderStatus == AVCENC_SUCCESS || encoderStatus == AVCENC_NEW_IDR) {
                    mReadyForNextFrame = false;
                    ++mNumInputFrames;
                    if (encoderStatus == AVCENC_NEW_IDR) {
                        mIsIDRFrame = 1;
                    }
                } else {
                    if (encoderStatus < AVCENC_SUCCESS) {
                        ALOGE("encoderStatus = %d at line %d", encoderStatus, __LINE__);
                        mSignalledError = true;
                        notify(OMX_EventError, OMX_ErrorUndefined, 0, 0);
                        return;
                    } else {
                        ALOGV("encoderStatus = %d at line %d", encoderStatus, __LINE__);
                        inQueue.erase(inQueue.begin());
                        inInfo->mOwnedByUs = false;
                        notifyEmptyBufferDone(inHeader);
                        return;
                    }
                }
            }
        }

        // Encode an input video frame
        CHECK(encoderStatus == AVCENC_SUCCESS || encoderStatus == AVCENC_NEW_IDR);
        dataLength = outHeader->nAllocLen;  // Reset the output buffer length
        if (inHeader->nFilledLen > 0) {
            if (outHeader->nAllocLen >= 4) {
                memcpy(outPtr, "\x00\x00\x00\x01", 4);
                outPtr += 4;
                dataLength -= 4;
            }
            encoderStatus = PVAVCEncodeNAL(mHandle, outPtr, &dataLength, &type);
            dataLength = outPtr + dataLength - outHeader->pBuffer;
            if (encoderStatus == AVCENC_SUCCESS) {
                CHECK(NULL == PVAVCEncGetOverrunBuffer(mHandle));
            } else if (encoderStatus == AVCENC_PICTURE_READY) {
                CHECK(NULL == PVAVCEncGetOverrunBuffer(mHandle));
                if (mIsIDRFrame) {
                    outHeader->nFlags |= OMX_BUFFERFLAG_SYNCFRAME;
                    mIsIDRFrame = false;
                }
                mReadyForNextFrame = true;
                AVCFrameIO recon;
                if (PVAVCEncGetRecon(mHandle, &recon) == AVCENC_SUCCESS) {
                    PVAVCEncReleaseRecon(mHandle, &recon);
                }
            } else {
                dataLength = 0;
                mReadyForNextFrame = true;
            }

            if (encoderStatus < AVCENC_SUCCESS) {
                ALOGE("encoderStatus = %d at line %d", encoderStatus, __LINE__);
                mSignalledError = true;
                notify(OMX_EventError, OMX_ErrorUndefined, 0, 0);
                return;
            }
        } else {
            dataLength = 0;
        }

        inQueue.erase(inQueue.begin());
        inInfo->mOwnedByUs = false;
        notifyEmptyBufferDone(inHeader);

        outQueue.erase(outQueue.begin());
        CHECK(!mInputBufferInfoVec.empty());
        InputBufferInfo *inputBufInfo = mInputBufferInfoVec.begin();
        outHeader->nTimeStamp = inputBufInfo->mTimeUs;
        outHeader->nFlags |= (inputBufInfo->mFlags | OMX_BUFFERFLAG_ENDOFFRAME);
        if (mSawInputEOS) {
            outHeader->nFlags |= OMX_BUFFERFLAG_EOS;
        }
        outHeader->nFilledLen = dataLength;
        outInfo->mOwnedByUs = false;
        notifyFillBufferDone(outHeader);
        mInputBufferInfoVec.erase(mInputBufferInfoVec.begin());
    }
}

int32_t SoftAVCEncoder::allocOutputBuffers(
        unsigned int sizeInMbs, unsigned int numBuffers) {
    CHECK(mOutputBuffers.isEmpty());
    size_t frameSize = (sizeInMbs << 7) * 3;
    for (unsigned int i = 0; i <  numBuffers; ++i) {
        MediaBuffer *buffer = new MediaBuffer(frameSize);
        buffer->setObserver(this);
        mOutputBuffers.push(buffer);
    }

    return 1;
}

void SoftAVCEncoder::unbindOutputBuffer(int32_t index) {
    CHECK(index >= 0);
}

int32_t SoftAVCEncoder::bindOutputBuffer(int32_t index, uint8_t **yuv) {
    CHECK(index >= 0);
    CHECK(index < (int32_t) mOutputBuffers.size());
    *yuv = (uint8_t *) mOutputBuffers[index]->data();

    return 1;
}

void SoftAVCEncoder::signalBufferReturned(MediaBuffer *buffer) {
    UNUSED_UNLESS_VERBOSE(buffer);
    ALOGV("signalBufferReturned: %p", buffer);
}

}  // namespace android

android::SoftOMXComponent *createSoftOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    return new android::SoftAVCEncoder(name, callbacks, appData, component);
}
