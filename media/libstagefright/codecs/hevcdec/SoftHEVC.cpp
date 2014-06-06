/*
 * Copyright (C) 2014 The Android Open Source Project
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
#define LOG_TAG "SoftHEVC"
#include <utils/Log.h>

#include "ihevc_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ithread.h"
#include "ihevcd_cxa.h"
#include "SoftHEVC.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <OMX_VideoExt.h>

namespace android {

#define componentName                   "video_decoder.hevc"
#define codingType                      OMX_VIDEO_CodingHEVC
#define CODEC_MIME_TYPE                 MEDIA_MIMETYPE_VIDEO_HEVC

/** Function and structure definitions to keep code similar for each codec */
#define ivdec_api_function              ihevcd_cxa_api_function
#define ivdext_init_ip_t                ihevcd_cxa_init_ip_t
#define ivdext_init_op_t                ihevcd_cxa_init_op_t
#define ivdext_fill_mem_rec_ip_t        ihevcd_cxa_fill_mem_rec_ip_t
#define ivdext_fill_mem_rec_op_t        ihevcd_cxa_fill_mem_rec_op_t
#define ivdext_ctl_set_num_cores_ip_t   ihevcd_cxa_ctl_set_num_cores_ip_t
#define ivdext_ctl_set_num_cores_op_t   ihevcd_cxa_ctl_set_num_cores_op_t

#define IVDEXT_CMD_CTL_SET_NUM_CORES    \
        (IVD_CONTROL_API_COMMAND_TYPE_T)IHEVCD_CXA_CMD_CTL_SET_NUM_CORES

static const CodecProfileLevel kProfileLevels[] = {
    { OMX_VIDEO_HEVCProfileMain, OMX_VIDEO_HEVCMainTierLevel1  },
    { OMX_VIDEO_HEVCProfileMain, OMX_VIDEO_HEVCMainTierLevel2  },
    { OMX_VIDEO_HEVCProfileMain, OMX_VIDEO_HEVCMainTierLevel21 },
    { OMX_VIDEO_HEVCProfileMain, OMX_VIDEO_HEVCMainTierLevel3  },
    { OMX_VIDEO_HEVCProfileMain, OMX_VIDEO_HEVCMainTierLevel31 },
    { OMX_VIDEO_HEVCProfileMain, OMX_VIDEO_HEVCMainTierLevel4  },
    { OMX_VIDEO_HEVCProfileMain, OMX_VIDEO_HEVCMainTierLevel41 },
    { OMX_VIDEO_HEVCProfileMain, OMX_VIDEO_HEVCMainTierLevel5  },
    { OMX_VIDEO_HEVCProfileMain, OMX_VIDEO_HEVCMainTierLevel51 },
};

SoftHEVC::SoftHEVC(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SoftVideoDecoderOMXComponent(name, componentName, codingType,
            kProfileLevels, ARRAY_SIZE(kProfileLevels),
            CODEC_MAX_WIDTH /* width */, CODEC_MAX_HEIGHT /* height */, callbacks,
            appData, component) {
    initPorts(kNumBuffers, INPUT_BUF_SIZE, kNumBuffers,
            CODEC_MIME_TYPE);

    mOmxColorFormat = OMX_COLOR_FormatYUV420Planar;
    mStride = mWidth;

    if (OMX_COLOR_FormatYUV420Planar == mOmxColorFormat) {
        mIvColorFormat = IV_YUV_420P;
    } else if (OMX_COLOR_FormatYUV420SemiPlanar == mOmxColorFormat) {
        mIvColorFormat = IV_YUV_420SP_UV;
    }

    mInitWidth = mWidth;
    mInitHeight = mHeight;

    CHECK_EQ(initDecoder(), (status_t)OK);
}

SoftHEVC::~SoftHEVC() {
    ALOGD("In SoftHEVC::~SoftHEVC");
    CHECK_EQ(deInitDecoder(), (status_t)OK);
}

static size_t GetCPUCoreCount() {
    long cpuCoreCount = 1;
#if defined(_SC_NPROCESSORS_ONLN)
    cpuCoreCount = sysconf(_SC_NPROCESSORS_ONLN);
#else
    // _SC_NPROC_ONLN must be defined...
    cpuCoreCount = sysconf(_SC_NPROC_ONLN);
#endif
    CHECK(cpuCoreCount >= 1);
    ALOGD("Number of CPU cores: %ld", cpuCoreCount);
    return (size_t)cpuCoreCount;
}

status_t SoftHEVC::getVersion() {
    ivd_ctl_getversioninfo_ip_t s_ctl_ip;
    ivd_ctl_getversioninfo_op_t s_ctl_op;
    UWORD8 au1_buf[512];
    IV_API_CALL_STATUS_T status;

    s_ctl_ip.e_cmd = IVD_CMD_VIDEO_CTL;
    s_ctl_ip.e_sub_cmd = IVD_CMD_CTL_GETVERSION;
    s_ctl_ip.u4_size = sizeof(ivd_ctl_getversioninfo_ip_t);
    s_ctl_op.u4_size = sizeof(ivd_ctl_getversioninfo_op_t);
    s_ctl_ip.pv_version_buffer = au1_buf;
    s_ctl_ip.u4_version_buffer_size = sizeof(au1_buf);

    status = ivdec_api_function(mCodecCtx, (void *)&s_ctl_ip,
            (void *)&s_ctl_op);

    if (status != IV_SUCCESS) {
        ALOGE("Error in getting version number: 0x%x",
                s_ctl_op.u4_error_code);
    } else {
        ALOGD("Ittiam decoder version number: %s",
                (char *)s_ctl_ip.pv_version_buffer);
    }
    return OK;
}

status_t SoftHEVC::setParams(WORD32 stride, IVD_VIDEO_DECODE_MODE_T decMode) {
    ivd_ctl_set_config_ip_t s_ctl_ip;
    ivd_ctl_set_config_op_t s_ctl_op;
    IV_API_CALL_STATUS_T status;
    s_ctl_ip.u4_disp_wd = stride;
    s_ctl_ip.e_frm_skip_mode = IVD_SKIP_NONE;

    s_ctl_ip.e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
    s_ctl_ip.e_vid_dec_mode = decMode;
    s_ctl_ip.e_cmd = IVD_CMD_VIDEO_CTL;
    s_ctl_ip.e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
    s_ctl_ip.u4_size = sizeof(ivd_ctl_set_config_ip_t);
    s_ctl_op.u4_size = sizeof(ivd_ctl_set_config_op_t);

    ALOGD("Set the run-time (dynamic) parameters");
    status = ivdec_api_function(mCodecCtx, (void *)&s_ctl_ip,
            (void *)&s_ctl_op);

    if (status != IV_SUCCESS) {
        ALOGE("Error in setting the run-time parameters: 0x%x",
                s_ctl_op.u4_error_code);

        return UNKNOWN_ERROR;
    }
    return OK;
}

status_t SoftHEVC::resetPlugin() {
    mIsInFlush = false;
    mReceivedEOS = false;
    memset(mTimeStamps, 0, sizeof(mTimeStamps));
    memset(mTimeStampsValid, 0, sizeof(mTimeStampsValid));

    /* Initialize both start and end times */
    gettimeofday(&mTimeStart, NULL);
    gettimeofday(&mTimeEnd, NULL);

    return OK;
}

status_t SoftHEVC::resetDecoder() {
    ivd_ctl_reset_ip_t s_ctl_ip;
    ivd_ctl_reset_op_t s_ctl_op;
    IV_API_CALL_STATUS_T status;

    s_ctl_ip.e_cmd = IVD_CMD_VIDEO_CTL;
    s_ctl_ip.e_sub_cmd = IVD_CMD_CTL_RESET;
    s_ctl_ip.u4_size = sizeof(ivd_ctl_reset_ip_t);
    s_ctl_op.u4_size = sizeof(ivd_ctl_reset_op_t);

    status = ivdec_api_function(mCodecCtx, (void *)&s_ctl_ip,
            (void *)&s_ctl_op);
    if (IV_SUCCESS != status) {
        ALOGE("Error in reset: 0x%x", s_ctl_op.u4_error_code);
        return UNKNOWN_ERROR;
    }

    /* Set the run-time (dynamic) parameters */
    setParams(0, IVD_DECODE_FRAME);

    /* Set number of cores/threads to be used by the codec */
    setNumCores();

    return OK;
}

status_t SoftHEVC::setNumCores() {
    ivdext_ctl_set_num_cores_ip_t s_set_cores_ip;
    ivdext_ctl_set_num_cores_op_t s_set_cores_op;
    IV_API_CALL_STATUS_T status;
    s_set_cores_ip.e_cmd = IVD_CMD_VIDEO_CTL;
    s_set_cores_ip.e_sub_cmd = IVDEXT_CMD_CTL_SET_NUM_CORES;
    s_set_cores_ip.u4_num_cores = MIN(mNumCores, CODEC_MAX_NUM_CORES);
    s_set_cores_ip.u4_size = sizeof(ivdext_ctl_set_num_cores_ip_t);
    s_set_cores_op.u4_size = sizeof(ivdext_ctl_set_num_cores_op_t);
    ALOGD("Set number of cores to %u", s_set_cores_ip.u4_num_cores);
    status = ivdec_api_function(mCodecCtx, (void *)&s_set_cores_ip,
            (void *)&s_set_cores_op);
    if (IV_SUCCESS != status) {
        ALOGE("Error in setting number of cores: 0x%x",
                s_set_cores_op.u4_error_code);
        return UNKNOWN_ERROR;
    }
    return OK;
}

status_t SoftHEVC::setFlushMode() {
    IV_API_CALL_STATUS_T status;
    ivd_ctl_flush_ip_t s_video_flush_ip;
    ivd_ctl_flush_op_t s_video_flush_op;

    s_video_flush_ip.e_cmd = IVD_CMD_VIDEO_CTL;
    s_video_flush_ip.e_sub_cmd = IVD_CMD_CTL_FLUSH;
    s_video_flush_ip.u4_size = sizeof(ivd_ctl_flush_ip_t);
    s_video_flush_op.u4_size = sizeof(ivd_ctl_flush_op_t);
    ALOGD("Set the decoder in flush mode ");

    /* Set the decoder in Flush mode, subsequent decode() calls will flush */
    status = ivdec_api_function(mCodecCtx, (void *)&s_video_flush_ip,
            (void *)&s_video_flush_op);

    if (status != IV_SUCCESS) {
        ALOGE("Error in setting the decoder in flush mode: (%d) 0x%x", status,
                s_video_flush_op.u4_error_code);
        return UNKNOWN_ERROR;
    }

    mIsInFlush = true;
    return OK;
}

status_t SoftHEVC::initDecoder() {
    IV_API_CALL_STATUS_T status;

    UWORD32 u4_num_reorder_frames;
    UWORD32 u4_num_ref_frames;
    UWORD32 u4_share_disp_buf;
    WORD32 i4_level;

    mNumCores = GetCPUCoreCount();

    /* Initialize number of ref and reorder modes (for HEVC) */
    u4_num_reorder_frames = 16;
    u4_num_ref_frames = 16;
    u4_share_disp_buf = 0;

    if ((mWidth * mHeight) > (1920 * 1088)) {
        i4_level = 50;
    } else if ((mWidth * mHeight) > (1280 * 720)) {
        i4_level = 41;
    } else {
        i4_level = 31;
    }

    {
        iv_num_mem_rec_ip_t s_num_mem_rec_ip;
        iv_num_mem_rec_op_t s_num_mem_rec_op;

        s_num_mem_rec_ip.u4_size = sizeof(s_num_mem_rec_ip);
        s_num_mem_rec_op.u4_size = sizeof(s_num_mem_rec_op);
        s_num_mem_rec_ip.e_cmd = IV_CMD_GET_NUM_MEM_REC;

        ALOGV("Get number of mem records");
        status = ivdec_api_function(mCodecCtx, (void*)&s_num_mem_rec_ip,
                (void*)&s_num_mem_rec_op);
        if (IV_SUCCESS != status) {
            ALOGE("Error in getting mem records: 0x%x",
                    s_num_mem_rec_op.u4_error_code);
            return UNKNOWN_ERROR;
        }

        mNumMemRecords = s_num_mem_rec_op.u4_num_mem_rec;
    }

    mMemRecords = (iv_mem_rec_t*)ivd_aligned_malloc(
            128, mNumMemRecords * sizeof(iv_mem_rec_t));
    if (mMemRecords == NULL) {
        ALOGE("Allocation failure");
        return NO_MEMORY;
    }

    {
        size_t i;
        ivdext_fill_mem_rec_ip_t s_fill_mem_ip;
        ivdext_fill_mem_rec_op_t s_fill_mem_op;
        iv_mem_rec_t *ps_mem_rec;

        s_fill_mem_ip.s_ivd_fill_mem_rec_ip_t.u4_size =
            sizeof(ivdext_fill_mem_rec_ip_t);
        s_fill_mem_ip.i4_level = i4_level;
        s_fill_mem_ip.u4_num_reorder_frames = u4_num_reorder_frames;
        s_fill_mem_ip.u4_num_ref_frames = u4_num_ref_frames;
        s_fill_mem_ip.u4_share_disp_buf = u4_share_disp_buf;
        s_fill_mem_ip.u4_num_extra_disp_buf = 0;
        s_fill_mem_ip.e_output_format = mIvColorFormat;

        s_fill_mem_ip.s_ivd_fill_mem_rec_ip_t.e_cmd = IV_CMD_FILL_NUM_MEM_REC;
        s_fill_mem_ip.s_ivd_fill_mem_rec_ip_t.pv_mem_rec_location = mMemRecords;
        s_fill_mem_ip.s_ivd_fill_mem_rec_ip_t.u4_max_frm_wd = mWidth;
        s_fill_mem_ip.s_ivd_fill_mem_rec_ip_t.u4_max_frm_ht = mHeight;
        s_fill_mem_op.s_ivd_fill_mem_rec_op_t.u4_size =
            sizeof(ivdext_fill_mem_rec_op_t);

        ps_mem_rec = mMemRecords;
        for (i = 0; i < mNumMemRecords; i++)
            ps_mem_rec[i].u4_size = sizeof(iv_mem_rec_t);

        status = ivdec_api_function(mCodecCtx, (void *)&s_fill_mem_ip,
                (void *)&s_fill_mem_op);

        if (IV_SUCCESS != status) {
            ALOGE("Error in filling mem records: 0x%x",
                    s_fill_mem_op.s_ivd_fill_mem_rec_op_t.u4_error_code);
            return UNKNOWN_ERROR;
        }
        mNumMemRecords =
            s_fill_mem_op.s_ivd_fill_mem_rec_op_t.u4_num_mem_rec_filled;

        ps_mem_rec = mMemRecords;

        for (i = 0; i < mNumMemRecords; i++) {
            ps_mem_rec->pv_base = ivd_aligned_malloc(
                    ps_mem_rec->u4_mem_alignment, ps_mem_rec->u4_mem_size);
            if (ps_mem_rec->pv_base == NULL) {
                ALOGE("Allocation failure for memory record #%zu of size %u",
                        i, ps_mem_rec->u4_mem_size);
                status = IV_FAIL;
                return NO_MEMORY;
            }

            ps_mem_rec++;
        }
    }

    /* Initialize the decoder */
    {
        ivdext_init_ip_t s_init_ip;
        ivdext_init_op_t s_init_op;

        void *dec_fxns = (void *)ivdec_api_function;

        s_init_ip.s_ivd_init_ip_t.u4_size = sizeof(ivdext_init_ip_t);
        s_init_ip.s_ivd_init_ip_t.e_cmd = (IVD_API_COMMAND_TYPE_T)IV_CMD_INIT;
        s_init_ip.s_ivd_init_ip_t.pv_mem_rec_location = mMemRecords;
        s_init_ip.s_ivd_init_ip_t.u4_frm_max_wd = mWidth;
        s_init_ip.s_ivd_init_ip_t.u4_frm_max_ht = mHeight;

        s_init_ip.i4_level = i4_level;
        s_init_ip.u4_num_reorder_frames = u4_num_reorder_frames;
        s_init_ip.u4_num_ref_frames = u4_num_ref_frames;
        s_init_ip.u4_share_disp_buf = u4_share_disp_buf;
        s_init_ip.u4_num_extra_disp_buf = 0;

        s_init_op.s_ivd_init_op_t.u4_size = sizeof(s_init_op);

        s_init_ip.s_ivd_init_ip_t.u4_num_mem_rec = mNumMemRecords;
        s_init_ip.s_ivd_init_ip_t.e_output_format = mIvColorFormat;

        mCodecCtx = (iv_obj_t*)mMemRecords[0].pv_base;
        mCodecCtx->pv_fxns = dec_fxns;
        mCodecCtx->u4_size = sizeof(iv_obj_t);

        ALOGD("Initializing decoder");
        status = ivdec_api_function(mCodecCtx, (void *)&s_init_ip,
                (void *)&s_init_op);
        if (status != IV_SUCCESS) {
            ALOGE("Error in init: 0x%x",
                    s_init_op.s_ivd_init_op_t.u4_error_code);
            return UNKNOWN_ERROR;
        }
    }

    /* Reset the plugin state */
    resetPlugin();

    /* Set the run time (dynamic) parameters */
    setParams(0, IVD_DECODE_FRAME);

    /* Set number of cores/threads to be used by the codec */
    setNumCores();

    /* Get codec version */
    getVersion();

    /* Allocate internal picture buffer */
    mFlushOutBuffer = (uint8_t *)ivd_aligned_malloc(128, mStride * mHeight * 3 / 2);
    if (NULL == mFlushOutBuffer) {
        ALOGE("Could not allocate flushOutputBuffer of size %zu", mStride * mHeight * 3 / 2);
        return NO_MEMORY;
    }

    return OK;
}

status_t SoftHEVC::deInitDecoder() {
    size_t i;
    iv_mem_rec_t *ps_mem_rec;
    ps_mem_rec = mMemRecords;
    ALOGD("Freeing codec memory");
    for (i = 0; i < mNumMemRecords; i++) {
        ivd_aligned_free(ps_mem_rec->pv_base);
        ps_mem_rec++;
    }

    ivd_aligned_free(mMemRecords);
    ivd_aligned_free(mFlushOutBuffer);
    return OK;
}

void SoftHEVC::onReset() {
    ALOGD("onReset called");
    SoftVideoDecoderOMXComponent::onReset();

    resetDecoder();
    resetPlugin();
}

void SoftHEVC::onPortFlushCompleted(OMX_U32 portIndex) {
    ALOGD("onPortFlushCompleted on port %d", portIndex);

    /* Once the output buffers are flushed, ignore any buffers that are held in decoder */
    if (kOutputPortIndex == portIndex) {
        setFlushMode();

        /* Reset the time stamp arrays */
        memset(mTimeStamps, 0, sizeof(mTimeStamps));
        memset(mTimeStampsValid, 0, sizeof(mTimeStampsValid));

        while (true) {
            ivd_video_decode_ip_t s_dec_ip;
            ivd_video_decode_op_t s_dec_op;
            IV_API_CALL_STATUS_T status;
            size_t sizeY, sizeUV;

            s_dec_ip.e_cmd = IVD_CMD_VIDEO_DECODE;

            s_dec_ip.u4_ts = 0;
            s_dec_ip.pv_stream_buffer = NULL;
            s_dec_ip.u4_num_Bytes = 0;

            s_dec_ip.u4_size = sizeof(ivd_video_decode_ip_t);
            s_dec_op.u4_size = sizeof(ivd_video_decode_op_t);

            sizeY = mStride * mHeight;
            sizeUV = sizeY / 4;
            s_dec_ip.s_out_buffer.u4_min_out_buf_size[0] = sizeY;
            s_dec_ip.s_out_buffer.u4_min_out_buf_size[1] = sizeUV;
            s_dec_ip.s_out_buffer.u4_min_out_buf_size[2] = sizeUV;

            s_dec_ip.s_out_buffer.pu1_bufs[0] = mFlushOutBuffer;
            s_dec_ip.s_out_buffer.pu1_bufs[1] =
                s_dec_ip.s_out_buffer.pu1_bufs[0] + sizeY;
            s_dec_ip.s_out_buffer.pu1_bufs[2] =
                s_dec_ip.s_out_buffer.pu1_bufs[1] + sizeUV;
            s_dec_ip.s_out_buffer.u4_num_bufs = 3;

            status = ivdec_api_function(mCodecCtx, (void *)&s_dec_ip,
                    (void *)&s_dec_op);
            if (0 == s_dec_op.u4_output_present) {
                resetPlugin();
                break;
            }
        }
    }
}

void SoftHEVC::onQueueFilled(OMX_U32 portIndex) {
    IV_API_CALL_STATUS_T status;

    UNUSED(portIndex);

    if (mOutputPortSettingsChange != NONE) {
        return;
    }

    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    /* If input EOS is seen and decoder is not in flush mode,
     * set the decoder in flush mode.
     * There can be a case where EOS is sent along with last picture data
     * In that case, only after decoding that input data, decoder has to be
     * put in flush. This case is handled here  */

    if (mReceivedEOS && !mIsInFlush) {
        setFlushMode();
    }

    while (outQueue.size() == kNumBuffers) {
        BufferInfo *inInfo;
        OMX_BUFFERHEADERTYPE *inHeader;

        BufferInfo *outInfo;
        OMX_BUFFERHEADERTYPE *outHeader;
        size_t timeStampIx;

        inInfo = NULL;
        inHeader = NULL;

        if (!mIsInFlush) {
            if (!inQueue.empty()) {
                inInfo = *inQueue.begin();
                inHeader = inInfo->mHeader;
            } else {
                break;
            }
        }

        outInfo = *outQueue.begin();
        outHeader = outInfo->mHeader;
        outHeader->nFlags = 0;
        outHeader->nTimeStamp = 0;
        outHeader->nOffset = 0;

        if (inHeader != NULL && (inHeader->nFlags & OMX_BUFFERFLAG_EOS)) {
            ALOGD("EOS seen on input");
            mReceivedEOS = true;
            if (inHeader->nFilledLen == 0) {
                inQueue.erase(inQueue.begin());
                inInfo->mOwnedByUs = false;
                notifyEmptyBufferDone(inHeader);
                inHeader = NULL;
                setFlushMode();
            }
        }

        /* Get a free slot in timestamp array to hold input timestamp */
        {
            size_t i;
            timeStampIx = 0;
            for (i = 0; i < MAX_TIME_STAMPS; i++) {
                if (!mTimeStampsValid[i]) {
                    timeStampIx = i;
                    break;
                }
            }
            if (inHeader != NULL) {
                mTimeStampsValid[timeStampIx] = true;
                mTimeStamps[timeStampIx] = inHeader->nTimeStamp;
            }
        }

        {
            ivd_video_decode_ip_t s_dec_ip;
            ivd_video_decode_op_t s_dec_op;
            WORD32 timeDelay, timeTaken;
            size_t sizeY, sizeUV;

            s_dec_ip.e_cmd = IVD_CMD_VIDEO_DECODE;

            /* When in flush and after EOS with zero byte input,
             * inHeader is set to zero. Hence check for non-null */
            if (inHeader != NULL) {
                s_dec_ip.u4_ts = timeStampIx;
                s_dec_ip.pv_stream_buffer = inHeader->pBuffer
                        + inHeader->nOffset;
                s_dec_ip.u4_num_Bytes = inHeader->nFilledLen;
            } else {
                s_dec_ip.u4_ts = 0;
                s_dec_ip.pv_stream_buffer = NULL;
                s_dec_ip.u4_num_Bytes = 0;
            }

            s_dec_ip.u4_size = sizeof(ivd_video_decode_ip_t);
            s_dec_op.u4_size = sizeof(ivd_video_decode_op_t);

            sizeY = mStride * mHeight;
            sizeUV = sizeY / 4;
            s_dec_ip.s_out_buffer.u4_min_out_buf_size[0] = sizeY;
            s_dec_ip.s_out_buffer.u4_min_out_buf_size[1] = sizeUV;
            s_dec_ip.s_out_buffer.u4_min_out_buf_size[2] = sizeUV;

            s_dec_ip.s_out_buffer.pu1_bufs[0] = outHeader->pBuffer;
            s_dec_ip.s_out_buffer.pu1_bufs[1] =
                s_dec_ip.s_out_buffer.pu1_bufs[0] + sizeY;
            s_dec_ip.s_out_buffer.pu1_bufs[2] =
                s_dec_ip.s_out_buffer.pu1_bufs[1] + sizeUV;
            s_dec_ip.s_out_buffer.u4_num_bufs = 3;

            GETTIME(&mTimeStart, NULL);
            /* Compute time elapsed between end of previous decode()
             * to start of current decode() */
            TIME_DIFF(mTimeEnd, mTimeStart, timeDelay);

            status = ivdec_api_function(mCodecCtx, (void *)&s_dec_ip,
                    (void *)&s_dec_op);

            GETTIME(&mTimeEnd, NULL);
            /* Compute time taken for decode() */
            TIME_DIFF(mTimeStart, mTimeEnd, timeTaken);

            ALOGD("timeTaken=%6d delay=%6d numBytes=%6d", timeTaken, timeDelay,
                    s_dec_op.u4_num_bytes_consumed);

            if ((inHeader != NULL) && (1 != s_dec_op.u4_frame_decoded_flag)) {
                /* If the input did not contain picture data, then ignore
                 * the associated timestamp */
                mTimeStampsValid[timeStampIx] = false;
            }

            /* If valid height and width are decoded,
             * then look at change in resolution */
            if ((0 < s_dec_op.u4_pic_wd) && (0 < s_dec_op.u4_pic_ht)) {
                uint32_t width = s_dec_op.u4_pic_wd;
                uint32_t height = s_dec_op.u4_pic_ht;

                if ((width != mWidth || height != mHeight)) {
                    mWidth = width;
                    mHeight = height;
                    mStride = mWidth;

                    /* If width and height are greater than the
                     * the dimensions used during codec create, then
                     * delete the current instance and recreate an instance with
                     * new dimensions */
                    /* TODO: The following does not work currently, since the decoder
                     * currently returns 0 x 0 as width height when it is not supported
                     * Once the decoder is updated to return actual width and height,
                     * then this can be validated*/

                    if ((mWidth * mHeight) > (mInitWidth * mInitHeight)) {
                        status_t ret;
                        ALOGD("Trying reInit");
                        ret = deInitDecoder();
                        if (OK != ret) {
                            // TODO: Handle graceful exit
                            ALOGE("Create failure");
                            return;
                        }

                        mInitWidth = mWidth;
                        mInitHeight = mHeight;

                        ret = initDecoder();
                        if (OK != ret) {
                            // TODO: Handle graceful exit
                            ALOGE("Create failure");
                            return;
                        }
                    }
                    updatePortDefinitions();

                    notify(OMX_EventPortSettingsChanged, 1, 0, NULL);
                    mOutputPortSettingsChange = AWAITING_DISABLED;
                    return;
                }
            }

            if (s_dec_op.u4_output_present) {
                outHeader->nFilledLen = (mStride * mHeight * 3) / 2;

                outHeader->nTimeStamp = mTimeStamps[s_dec_op.u4_ts];
                mTimeStampsValid[s_dec_op.u4_ts] = false;

                outInfo->mOwnedByUs = false;
                outQueue.erase(outQueue.begin());
                outInfo = NULL;
                notifyFillBufferDone(outHeader);
                outHeader = NULL;
            } else {
                /* If in flush mode and no output is returned by the codec,
                 * then come out of flush mode */
                mIsInFlush = false;

                /* If EOS was recieved on input port and there is no output
                 * from the codec, then signal EOS on output port */
                if (mReceivedEOS) {
                    outHeader->nFilledLen = 0;
                    outHeader->nFlags |= OMX_BUFFERFLAG_EOS;

                    outInfo->mOwnedByUs = false;
                    outQueue.erase(outQueue.begin());
                    outInfo = NULL;
                    notifyFillBufferDone(outHeader);
                    outHeader = NULL;
                    resetPlugin();
                }
            }
        }

        // TODO: Handle more than one picture data
        if (inHeader != NULL) {
            inInfo->mOwnedByUs = false;
            inQueue.erase(inQueue.begin());
            inInfo = NULL;
            notifyEmptyBufferDone(inHeader);
            inHeader = NULL;
        }
    }
}

} // namespace android

android::SoftOMXComponent *createSoftOMXComponent(const char *name,
        const OMX_CALLBACKTYPE *callbacks, OMX_PTR appData,
        OMX_COMPONENTTYPE **component) {
    return new android::SoftHEVC(name, callbacks, appData, component);
}
