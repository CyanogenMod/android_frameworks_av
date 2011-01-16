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
/**
*************************************************************************
* @file   VideoEditorVideoDecoder.cpp
* @brief  StageFright shell video decoder
*************************************************************************
*/
#define LOG_NDEBUG 1
#define LOG_TAG "VIDEOEDITOR_VIDEODECODER"

/*******************
 *     HEADERS     *
 *******************/

#include "VideoEditorVideoDecoder_internal.h"
#include "VideoEditorUtils.h"
#include "M4VD_Tools.h"

#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaDefs.h>

/********************
 *   DEFINITIONS    *
 ********************/
#define OMX_QCOM_COLOR_FormatYVU420SemiPlanar 0x7FA30C00
#define MAX_DEC_BUFFERS 10

/********************
 *   SOURCE CLASS   *
 ********************/
using namespace android;

class VideoEditorVideoDecoderSource : public MediaSource {
    public:
        VideoEditorVideoDecoderSource(const sp<MetaData> &format,
            VIDEOEDITOR_CodecType codecType, void *decoderShellContext);
        virtual status_t start(MetaData *params = NULL);
        virtual status_t stop();
        virtual sp<MetaData> getFormat();
        virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options = NULL);

    protected :
        virtual ~VideoEditorVideoDecoderSource();

    private:
        sp<MetaData> mFormat;
        MediaBuffer* mBuffer;
        MediaBufferGroup* mGroup;
        Mutex mLock;
        VideoEditorVideoDecoder_Context* mpDecShellContext;
        int32_t mMaxAUSize;
        bool mStarted;
        VIDEOEDITOR_CodecType mCodecType;
        VideoEditorVideoDecoderSource(const MediaSource &);
        VideoEditorVideoDecoderSource &operator=(const MediaSource &);
};

VideoEditorVideoDecoderSource::VideoEditorVideoDecoderSource(
        const sp<MetaData> &format, VIDEOEDITOR_CodecType codecType,
        void *decoderShellContext) :
        mFormat(format),
        mBuffer(NULL),
        mGroup(NULL),
        mStarted(false),
        mCodecType(codecType) {
    mpDecShellContext = (VideoEditorVideoDecoder_Context*) decoderShellContext;
}

VideoEditorVideoDecoderSource::~VideoEditorVideoDecoderSource() {
    if (mStarted == true) {
        stop();
    }
}

status_t VideoEditorVideoDecoderSource::start(
        MetaData *params) {

    LOGV("VideoEditorVideoDecoderSource::start() begin ");
    if (!mStarted) {
        if(mFormat->findInt32(kKeyMaxInputSize, &mMaxAUSize) == false) {
            LOGW("FATAL: Should never happen ");
            mMaxAUSize = 10000;
        }

        mGroup = new MediaBufferGroup;
        if(mGroup == NULL) {
            LOGE("FATAL: memory limitation ! ");
            return NO_MEMORY;
        }
        LOGV("VideoEditorVideoDecoderSource:adding buffer to group MaxSize= %d",
            mMaxAUSize);
        mGroup->add_buffer(new MediaBuffer(mMaxAUSize));

        mStarted = true;
    }
    LOGV("VideoEditorVideoDecoderSource::start() end OK");
    return OK;
}

status_t VideoEditorVideoDecoderSource::stop() {
    int ref_count = 0;
    int i;

    LOGV("VideoEditorVideoDecoderSource::stop() begin");
    if (mStarted) {
        if(mBuffer != NULL) {
            ref_count = mBuffer->refcount();
            LOGV("MediaBuffer refcount is %d",ref_count);
            for (i=0; i< ref_count; i++) {
                mBuffer->release();
            }

            mBuffer = NULL;
        }
        delete mGroup;
        mGroup = NULL;
        mStarted = false;
    }
    LOGV("VideoEditorVideoDecoderSource::stop() end");
    return OK;
}

sp<MetaData> VideoEditorVideoDecoderSource::getFormat() {
    Mutex::Autolock autolock(mLock);

    return mFormat;
}

status_t VideoEditorVideoDecoderSource::read(MediaBuffer** buffer_out,
        const ReadOptions *options) {

    Mutex::Autolock autolock(mLock);
    //We donot use read options on decoder hence dont impliment this option here
    M4_AccessUnit* pAccessUnit = mpDecShellContext->m_pNextAccessUnitToDecode;
    M4OSA_UInt32 lSize = 0;
    M4OSA_ERR lerr = M4NO_ERROR;
    int64_t frameTime;

    *buffer_out = NULL;

    LOGV("VideoEditorVideoDecoderSource::read begin");

    if (options) {
        int64_t time = 0;
        ReadOptions::SeekMode mode = ReadOptions::SEEK_CLOSEST_SYNC;
        bool hasOptions = FALSE;
        hasOptions = options->getSeekTo(&time, &mode);
        if (hasOptions) {
            LOGV("VideoEditorVideoDecoderSource: Options is not NULL  %lld %d",
                time, mode);
        } else {
            LOGV("VideoEditorVideoDecoderSource: Options is not NULL ****");
        }
    }
    lerr = mGroup->acquire_buffer(&mBuffer);
    if (lerr != OK) {
        return lerr;
    }
    LOGV("VideoEditorVideoDecoderSource: got a buffer from group");

    if (mStarted) {
        //getNext AU from reader.
        lerr = mpDecShellContext->m_pReader->m_pFctGetNextAu(
                   mpDecShellContext->m_pReader->m_readerContext,
                   (M4_StreamHandler*)mpDecShellContext->m_pVideoStreamhandler,
                   pAccessUnit);
        if (lerr == M4WAR_NO_DATA_YET) {
            LOGV("VideoEditorVideoDecoderSource::read() M4WAR_NO_DATA_YET");
            mBuffer->set_range(0, 0);
            mBuffer->meta_data()->clear();

            *buffer_out = mBuffer;
        }
        if (lerr == M4WAR_NO_MORE_AU) {
            LOGV("VideoEditorVideoDecoderSource::read() returning err = "
                "ERROR_END_OF_STREAM;");
            *buffer_out = NULL;
            return ERROR_END_OF_STREAM;
        }
        LOGV("VideoEditorVideoDecoderSource: getNextAU  succesful ts = %lf",
            pAccessUnit->m_CTS);

        //copy the reader AU buffer to mBuffer
        lSize  = (pAccessUnit->m_size > (M4OSA_UInt32)mMaxAUSize)\
            ? (M4OSA_UInt32)mMaxAUSize : pAccessUnit->m_size;
        LOGV("VideoDecoderSource:Read() copying AU to i/p buffer of decoder,"
            "Bufer Add = 0x%x, size = %d", mBuffer->data(), lSize);
        M4OSA_memcpy((M4OSA_MemAddr8)mBuffer->data(),pAccessUnit->m_dataAddress,
            lSize);

        mBuffer->set_range(0, lSize);
        mBuffer->meta_data()->clear();
        frameTime = (int64_t)pAccessUnit->m_CTS;
        mBuffer->meta_data()->setInt64(kKeyTime, (int64_t)frameTime*1000);

        // Replace the AU start code for H264
        if (VIDEOEDITOR_kH264VideoDec == mCodecType) {
            uint8_t *data =(uint8_t *)mBuffer->data() + mBuffer->range_offset();
            data[0]=0;
            data[1]=0;
            data[2]=0;
            data[3]=1;
        }
        mBuffer->meta_data()->setInt32(kKeyIsSyncFrame,
            (pAccessUnit->m_attribute == 0x04)? 1 : 0);
        *buffer_out = mBuffer;
    }
    LOGV("VideoEditorVideoDecoderSource::read end");
    return OK;
}
/********************
 *      TOOLS       *
 ********************/

static M4OSA_UInt32 VideoEditorVideoDecoder_GetBitsFromMemory(
        VIDEOEDITOR_VIDEO_Bitstream_ctxt* parsingCtxt, M4OSA_UInt32 nb_bits) {
    return (M4VD_Tools_GetBitsFromMemory((M4VS_Bitstream_ctxt*) parsingCtxt,
            nb_bits));
}

M4OSA_ERR VideoEditorVideoDecoder_internalParseVideoDSI(M4OSA_UInt8* pVol,
        M4OSA_Int32 aVolSize, M4DECODER_MPEG4_DecoderConfigInfo* pDci,
        M4DECODER_VideoSize* pVideoSize) {

    VIDEOEDITOR_VIDEO_Bitstream_ctxt parsingCtxt;
    M4OSA_UInt32 code, j;
    M4OSA_MemAddr8 start;
    M4OSA_UInt8 i;
    M4OSA_UInt32 time_incr_length;
    M4OSA_UInt8 vol_verid=0, b_hierarchy_type;

    /* Parsing variables */
    M4OSA_UInt8 video_object_layer_shape = 0;
    M4OSA_UInt8 sprite_enable = 0;
    M4OSA_UInt8 reduced_resolution_vop_enable = 0;
    M4OSA_UInt8 scalability = 0;
    M4OSA_UInt8 enhancement_type = 0;
    M4OSA_UInt8 complexity_estimation_disable = 0;
    M4OSA_UInt8 interlaced = 0;
    M4OSA_UInt8 sprite_warping_points = 0;
    M4OSA_UInt8 sprite_brightness_change = 0;
    M4OSA_UInt8 quant_precision = 0;

    /* Fill the structure with default parameters */
    pVideoSize->m_uiWidth      = 0;
    pVideoSize->m_uiHeight     = 0;

    pDci->uiTimeScale          = 0;
    pDci->uiProfile            = 0;
    pDci->uiUseOfResynchMarker = 0;
    pDci->bDataPartition       = M4OSA_FALSE;
    pDci->bUseOfRVLC           = M4OSA_FALSE;

    /* Reset the bitstream context */
    parsingCtxt.stream_byte = 0;
    parsingCtxt.stream_index = 8;
    parsingCtxt.in = (M4OSA_MemAddr8) pVol;

    start = (M4OSA_MemAddr8) pVol;

    /* Start parsing */
    while (parsingCtxt.in - start < aVolSize) {
        code = VideoEditorVideoDecoder_GetBitsFromMemory(&parsingCtxt, 8);
        if (code == 0) {
            code = VideoEditorVideoDecoder_GetBitsFromMemory(&parsingCtxt, 8);
            if (code == 0) {
                code = VideoEditorVideoDecoder_GetBitsFromMemory(&parsingCtxt,8);
                if (code == 1) {
                    /* start code found */
                    code = VideoEditorVideoDecoder_GetBitsFromMemory(
                        &parsingCtxt, 8);

                    /* ----- 0x20..0x2F : video_object_layer_start_code ----- */

                    if ((code > 0x1F) && (code < 0x30)) {
                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);
                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 8);
                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);
                        if (code == 1) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 4);
                            vol_verid = (M4OSA_UInt8)code;
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 3);
                        }
                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 4);
                        if (code == 15) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 16);
                        }
                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);
                        if (code == 1) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 3);
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 1);
                            if (code == 1) {
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 32);
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 31);
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 16);
                            }
                        }
                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 2);
                        /* Need to save it for vop parsing */
                        video_object_layer_shape = (M4OSA_UInt8)code;

                        if (code != 0) {
                            return 0;    /* only rectangular case supported */
                        }

                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);
                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 16);
                        pDci->uiTimeScale = code;

                        /* Computes time increment length */
                        j    = code - 1;
                        for (i = 0; (i < 32) && (j != 0); j >>=1) {
                            i++;
                        }
                        time_incr_length = (i == 0) ? 1 : i;

                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);
                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);
                        if (code == 1) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, time_incr_length);
                        }

                        if(video_object_layer_shape != 1) { /* 1 = Binary */
                            if(video_object_layer_shape == 0) {
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* Marker bit */
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 13);/* Width */
                                pVideoSize->m_uiWidth = code;
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* Marker bit */
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 13);/* Height */
                                pVideoSize->m_uiHeight = code;
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* Marker bit */
                            }
                        }

                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);/* interlaced */
                        interlaced = (M4OSA_UInt8)code;
                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);/* OBMC disable */

                        if(vol_verid == 1) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 1);/* sprite enable */
                            sprite_enable = (M4OSA_UInt8)code;
                        } else {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 2);/* sprite enable */
                            sprite_enable = (M4OSA_UInt8)code;
                        }
                        if ((sprite_enable == 1) || (sprite_enable == 2)) {
                            if (sprite_enable != 2) {

                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 13);/* sprite width */
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* Marker bit */
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 13);/* sprite height */
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* Marker bit */
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 13);/* sprite l coordinate */
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* Marker bit */
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 13);/* sprite top coordinate */
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* Marker bit */
                            }

                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 6);/* sprite warping points */
                            sprite_warping_points = (M4OSA_UInt8)code;
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 2);/* sprite warping accuracy */
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 1);/* sprite brightness change */
                            sprite_brightness_change = (M4OSA_UInt8)code;
                            if (sprite_enable != 2) {
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 1);
                            }
                        }
                        if ((vol_verid != 1) && (video_object_layer_shape != 0)){
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);/* sadct disable */
                        }

                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1); /* not 8 bits */
                        if (code) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 4);/* quant precision */
                            quant_precision = (M4OSA_UInt8)code;
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 4);/* bits per pixel */
                        }

                        /* greyscale not supported */
                        if(video_object_layer_shape == 3) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 3);
                        }

                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);/* quant type */
                        if (code) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 1);/* load intra quant mat */
                            if (code) {
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 8);/* */
                                i    = 1;
                                while (i < 64) {
                                    code =
                                        VideoEditorVideoDecoder_GetBitsFromMemory(
                                            &parsingCtxt, 8);
                                    if (code == 0) {
                                        break;
                                    }
                                    i++;
                                }
                            }

                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 1);/* load non intra quant mat */
                            if (code) {
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 8);/* */
                                i    = 1;
                                while (i < 64) {
                                    code =
                                        VideoEditorVideoDecoder_GetBitsFromMemory(
                                        &parsingCtxt, 8);
                                    if (code == 0) {
                                        break;
                                    }
                                    i++;
                                }
                            }
                        }

                        if (vol_verid != 1) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 1);/* quarter sample */
                        }

                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);/* complexity estimation disable */
                        complexity_estimation_disable = (M4OSA_UInt8)code;
                        if (!code) {
                            //return M4ERR_NOT_IMPLEMENTED;
                        }

                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);/* resync marker disable */
                        pDci->uiUseOfResynchMarker = (code) ? 0 : 1;

                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);/* data partitionned */
                        pDci->bDataPartition = (code) ? M4OSA_TRUE : M4OSA_FALSE;
                        if (code) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 1);/* reversible VLC */
                            pDci->bUseOfRVLC = (code) ? M4OSA_TRUE : M4OSA_FALSE;
                        }

                        if (vol_verid != 1) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 1);/* newpred */
                            if (code) {
                                //return M4ERR_PARAMETER;
                            }

                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 1);
                            reduced_resolution_vop_enable = (M4OSA_UInt8)code;
                        }

                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);/* scalability */
                        scalability = (M4OSA_UInt8)code;
                        if (code) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 1);/* hierarchy type */
                            b_hierarchy_type = (M4OSA_UInt8)code;
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 4);/* ref layer id */
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 1);/* ref sampling direct */
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 5);/* hor sampling factor N */
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 5);/* hor sampling factor M */
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 5);/* vert sampling factor N */
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 5);/* vert sampling factor M */
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 1);/* enhancement type */
                            enhancement_type = (M4OSA_UInt8)code;
                            if ((!b_hierarchy_type) &&
                                    (video_object_layer_shape == 1)) {
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* use ref shape */
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* use ref texture */
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 5);
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 5);
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 5);
                                code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                    &parsingCtxt, 5);
                            }
                        }
                        break;
                    }

                    /* ----- 0xB0 : visual_object_sequence_start_code ----- */

                    else if(code == 0xB0) {
                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 8);/* profile_and_level_indication */
                        pDci->uiProfile = (M4OSA_UInt8)code;
                    }

                    /* ----- 0xB5 : visual_object_start_code ----- */

                    else if(code == 0xB5) {
                        code = VideoEditorVideoDecoder_GetBitsFromMemory(
                            &parsingCtxt, 1);/* is object layer identifier */
                        if (code == 1) {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 4); /* visual object verid */
                            vol_verid = (M4OSA_UInt8)code;
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 3);
                        } else {
                            code = VideoEditorVideoDecoder_GetBitsFromMemory(
                                &parsingCtxt, 7); /* Realign on byte */
                            vol_verid = 1;
                        }
                    }

                    /* ----- end ----- */
                } else {
                    if ((code >> 2) == 0x20) {
                        /* H263 ...-> wrong*/
                        break;
                    }
                }
            }
        }
    }
    return M4NO_ERROR;
}

M4VIFI_UInt8 M4VIFI_SemiplanarYVU420toYUV420(void *user_data,
        M4VIFI_UInt8 *inyuv, M4VIFI_ImagePlane *PlaneOut ) {
    M4VIFI_UInt8 return_code = M4VIFI_OK;
    M4VIFI_UInt8 *outyuv =
        ((M4VIFI_UInt8*)&(PlaneOut[0].pac_data[PlaneOut[0].u_topleft]));
    int32_t width = PlaneOut[0].u_width;
    int32_t height = PlaneOut[0].u_height;

    int32_t outYsize = width * height;
    uint32_t *outy =  (uint32_t *) outyuv;
    uint16_t *outcb =
        (uint16_t *) &(PlaneOut[1].pac_data[PlaneOut[1].u_topleft]);
    uint16_t *outcr =
        (uint16_t *) &(PlaneOut[2].pac_data[PlaneOut[2].u_topleft]);

    /* Y copying */
    memcpy(outy, inyuv, outYsize);

    /* U & V copying */
    uint32_t *inyuv_4 = (uint32_t *) (inyuv + outYsize);
    for (int32_t i = height >> 1; i > 0; --i) {
        for (int32_t j = width >> 2; j > 0; --j) {
            uint32_t temp = *inyuv_4++;
            uint32_t tempU = temp & 0xFF;
            tempU = tempU | ((temp >> 8) & 0xFF00);

            uint32_t tempV = (temp >> 8) & 0xFF;
            tempV = tempV | ((temp >> 16) & 0xFF00);

            // Flip U and V
            *outcb++ = tempV;
            *outcr++ = tempU;
        }
    }
    return return_code;
}

M4OSA_ERR VideoEditorVideoDecoder_ParseAVCDSI(M4OSA_UInt8* pDSI,
        M4OSA_Int32 DSISize, M4DECODER_AVCProfileLevel *profile) {
    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_Bool NALSPS_and_Profile0Found = M4OSA_FALSE;
    M4OSA_UInt16 index;
    M4OSA_Bool constraintSet3;

    for(index = 0; index < (DSISize-1); index++) {
        if(((pDSI[index] & 0x1f) == 0x07) && (pDSI[index+1] == 0x42)) {
            NALSPS_and_Profile0Found = M4OSA_TRUE;
            break;
        }
    }
    if(M4OSA_FALSE == NALSPS_and_Profile0Found) {
        LOGV("VideoEditorVideoDecoder_ParseAVCDSI: index bad = %d", index);
        *profile = M4DECODER_AVC_kProfile_and_Level_Out_Of_Range;
    } else {
        LOGV("VideoEditorVideoDecoder_ParseAVCDSI: index = %d", index);
        constraintSet3 = (pDSI[index+2] & 0x10);
        LOGV("VideoEditorVideoDecoder_ParseAVCDSI: level = %d", pDSI[index+3]);
        switch(pDSI[index+3]) {
            case 10:
                *profile = M4DECODER_AVC_kProfile_0_Level_1;
                break;
            case 11:
                if(constraintSet3) {
                    *profile = M4DECODER_AVC_kProfile_0_Level_1b;
                } else {
                    *profile = M4DECODER_AVC_kProfile_0_Level_1_1;
                }
                break;
            case 12:
                *profile = M4DECODER_AVC_kProfile_0_Level_1_2;
                break;
            case 13:
                *profile = M4DECODER_AVC_kProfile_0_Level_1_3;
                break;
            case 20:
                *profile = M4DECODER_AVC_kProfile_0_Level_2;
                break;
            case 21:
                *profile = M4DECODER_AVC_kProfile_0_Level_2_1;
                break;
            case 22:
                *profile = M4DECODER_AVC_kProfile_0_Level_2_2;
                break;
            case 30:
                *profile = M4DECODER_AVC_kProfile_0_Level_3;
                break;
            case 31:
                *profile = M4DECODER_AVC_kProfile_0_Level_3_1;
                break;
            case 32:
                *profile = M4DECODER_AVC_kProfile_0_Level_3_2;
                break;
            case 40:
                *profile = M4DECODER_AVC_kProfile_0_Level_4;
                break;
            case 41:
                *profile = M4DECODER_AVC_kProfile_0_Level_4_1;
                break;
            case 42:
                *profile = M4DECODER_AVC_kProfile_0_Level_4_2;
                break;
            case 50:
                *profile = M4DECODER_AVC_kProfile_0_Level_5;
                break;
            case 51:
                *profile = M4DECODER_AVC_kProfile_0_Level_5_1;
                break;
            default:
                *profile = M4DECODER_AVC_kProfile_and_Level_Out_Of_Range;
        }
    }
    return err;
}
/********************
 * ENGINE INTERFACE *
 ********************/
M4OSA_ERR VideoEditorVideoDecoder_configureFromMetadata(M4OSA_Context pContext,
        MetaData* meta) {
    M4OSA_ERR err = M4NO_ERROR;
    VideoEditorVideoDecoder_Context* pDecShellContext = M4OSA_NULL;
    bool success = OK;
    int32_t width = 0;
    int32_t height = 0;
    int32_t frameSize = 0;

    VIDEOEDITOR_CHECK(M4OSA_NULL != pContext, M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK(M4OSA_NULL != meta,     M4ERR_PARAMETER);

    LOGV("VideoEditorVideoDecoder_configureFromMetadata begin");

    pDecShellContext = (VideoEditorVideoDecoder_Context*)pContext;

    // Get the parameters
    success  = meta->findInt32(kKeyWidth,  &width);
    success &= meta->findInt32(kKeyHeight, &height);
    VIDEOEDITOR_CHECK(TRUE == success, M4ERR_PARAMETER);

    LOGV("VideoDecoder_configureFromMetadata : W=%d H=%d", width, height);
    VIDEOEDITOR_CHECK((0 != width) && (0 != height), M4ERR_PARAMETER);

    LOGV("VideoDecoder_configureFromMetadata : W=%d H=%d", width, height);

    if( (M4OSA_NULL != pDecShellContext->m_pDecBufferPool) &&
        (pDecShellContext->m_pVideoStreamhandler->m_videoWidth  == \
            (uint32_t)width) &&
        (pDecShellContext->m_pVideoStreamhandler->m_videoHeight == \
            (uint32_t)height) ) {
        // No need to reconfigure
        goto cleanUp;
    }
    LOGV("VideoDecoder_configureFromMetadata  reset: W=%d H=%d", width, height);
    // Update the stream handler parameters
    pDecShellContext->m_pVideoStreamhandler->m_videoWidth  = width;
    pDecShellContext->m_pVideoStreamhandler->m_videoHeight = height;
    frameSize = (width * height * 3) / 2;

    // Configure the buffer pool
    if( M4OSA_NULL != pDecShellContext->m_pDecBufferPool ) {
        LOGV("VideoDecoder_configureFromMetadata : reset the buffer pool");
        VIDEOEDITOR_BUFFER_freePool(pDecShellContext->m_pDecBufferPool);
        pDecShellContext->m_pDecBufferPool = M4OSA_NULL;
    }
    err =  VIDEOEDITOR_BUFFER_allocatePool(&pDecShellContext->m_pDecBufferPool,
        MAX_DEC_BUFFERS, (M4OSA_Char*)"VIDEOEDITOR_DecodedBufferPool");
    VIDEOEDITOR_CHECK(M4NO_ERROR == err, err);
    err = VIDEOEDITOR_BUFFER_initPoolBuffers(pDecShellContext->m_pDecBufferPool,
        frameSize + width * 2);
    VIDEOEDITOR_CHECK(M4NO_ERROR == err, err);

cleanUp:
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorVideoDecoder_configureFromMetadata no error");
    } else {
        if( M4OSA_NULL != pDecShellContext->m_pDecBufferPool ) {
            VIDEOEDITOR_BUFFER_freePool(pDecShellContext->m_pDecBufferPool);
            pDecShellContext->m_pDecBufferPool = M4OSA_NULL;
        }
        LOGV("VideoEditorVideoDecoder_configureFromMetadata ERROR 0x%X", err);
    }
    LOGV("VideoEditorVideoDecoder_configureFromMetadata end");
    return err;
}

M4OSA_ERR VideoEditorVideoDecoder_destroy(M4OSA_Context pContext) {
    M4OSA_ERR err = M4NO_ERROR;
    VideoEditorVideoDecoder_Context* pDecShellContext =
        (VideoEditorVideoDecoder_Context*)pContext;

    // Input parameters check
    LOGV("VideoEditorVideoDecoder_destroy begin");
    VIDEOEDITOR_CHECK(M4OSA_NULL != pContext, M4ERR_PARAMETER);

    // Destroy the graph
    if( pDecShellContext->mVideoDecoder != NULL ) {
        LOGV("### VideoEditorVideoDecoder_destroy : releasing decoder");
        pDecShellContext->mVideoDecoder->stop();
        pDecShellContext->mVideoDecoder.clear();
    }
    pDecShellContext->mClient.disconnect();
    pDecShellContext->mReaderSource.clear();

    // Release memory
    if( pDecShellContext->m_pDecBufferPool != M4OSA_NULL ) {
        VIDEOEDITOR_BUFFER_freePool(pDecShellContext->m_pDecBufferPool);
        pDecShellContext->m_pDecBufferPool = M4OSA_NULL;
    }
    SAFE_FREE(pDecShellContext);
    pContext = NULL;

cleanUp:
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorVideoDecoder_destroy no error");
    } else {
        LOGV("VideoEditorVideoDecoder_destroy ERROR 0x%X", err);
    }
    LOGV("VideoEditorVideoDecoder_destroy end");
    return err;
}

M4OSA_ERR VideoEditorVideoDecoder_create(M4OSA_Context *pContext,
        M4_StreamHandler *pStreamHandler,
        M4READER_DataInterface *pReaderDataInterface,
        M4_AccessUnit *pAccessUnit, M4OSA_Void *pUserData) {
    M4OSA_ERR err = M4NO_ERROR;
    VideoEditorVideoDecoder_Context* pDecShellContext = M4OSA_NULL;
    status_t status = OK;
    bool success = TRUE;
    int32_t colorFormat = 0;
    M4OSA_UInt32 size = 0;
    sp<MetaData> decoderMetadata = NULL;

    LOGV("VideoEditorVideoDecoder_create begin");
    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != pContext,             M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK(M4OSA_NULL != pStreamHandler,       M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK(M4OSA_NULL != pReaderDataInterface, M4ERR_PARAMETER);

    // Context allocation & initialization
    SAFE_MALLOC(pDecShellContext, VideoEditorVideoDecoder_Context, 1,
        "VideoEditorVideoDecoder");
    pDecShellContext->m_pVideoStreamhandler =
        (M4_VideoStreamHandler*)pStreamHandler;
    pDecShellContext->m_pNextAccessUnitToDecode = pAccessUnit;
    pDecShellContext->m_pReader = pReaderDataInterface;
    pDecShellContext->m_lastDecodedCTS = -1;
    pDecShellContext->m_lastRenderCts = -1;
    switch( pStreamHandler->m_streamType ) {
        case M4DA_StreamTypeVideoH263:
            pDecShellContext->mDecoderType = VIDEOEDITOR_kH263VideoDec;
            break;
        case M4DA_StreamTypeVideoMpeg4:
            pDecShellContext->mDecoderType = VIDEOEDITOR_kMpeg4VideoDec;
            // Parse the VOL header
            err = VideoEditorVideoDecoder_internalParseVideoDSI(
                (M4OSA_UInt8*)pDecShellContext->m_pVideoStreamhandler->\
                    m_basicProperties.m_pDecoderSpecificInfo,
                pDecShellContext->m_pVideoStreamhandler->\
                    m_basicProperties.m_decoderSpecificInfoSize,
                &pDecShellContext->m_Dci, &pDecShellContext->m_VideoSize);
            VIDEOEDITOR_CHECK(M4NO_ERROR == err, err);
            break;
        case M4DA_StreamTypeVideoMpeg4Avc:
            pDecShellContext->mDecoderType = VIDEOEDITOR_kH264VideoDec;
            break;
        default:
            VIDEOEDITOR_CHECK(!"VideoDecoder_create : incorrect stream type",
                M4ERR_PARAMETER);
            break;
    }

    pDecShellContext->mNbInputFrames     = 0;
    pDecShellContext->mFirstInputCts     = -1.0;
    pDecShellContext->mLastInputCts      = -1.0;
    pDecShellContext->mNbRenderedFrames  = 0;
    pDecShellContext->mFirstRenderedCts  = -1.0;
    pDecShellContext->mLastRenderedCts   = -1.0;
    pDecShellContext->mNbOutputFrames    = 0;
    pDecShellContext->mFirstOutputCts    = -1;
    pDecShellContext->mLastOutputCts     = -1;

    /**
     * StageFright graph building
     */
    decoderMetadata = new MetaData;
    switch( pDecShellContext->mDecoderType ) {
        case VIDEOEDITOR_kH263VideoDec:
            decoderMetadata->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_H263);
            break;
        case VIDEOEDITOR_kMpeg4VideoDec:
            decoderMetadata->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG4);
            decoderMetadata->setData(kKeyESDS, kTypeESDS,
                pStreamHandler->m_pESDSInfo,
                pStreamHandler->m_ESDSInfoSize);
            break;
        case VIDEOEDITOR_kH264VideoDec:
            decoderMetadata->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_AVC);
            decoderMetadata->setData(kKeyAVCC, kTypeAVCC,
                pStreamHandler->m_pH264DecoderSpecificInfo,
                pStreamHandler->m_H264decoderSpecificInfoSize);
            break;
        default:
            VIDEOEDITOR_CHECK(!"VideoDecoder_create : incorrect stream type",
                M4ERR_PARAMETER);
            break;
    }

    decoderMetadata->setInt32(kKeyMaxInputSize, pStreamHandler->m_maxAUSize);
    decoderMetadata->setInt32(kKeyWidth,
        pDecShellContext->m_pVideoStreamhandler->m_videoWidth);
    decoderMetadata->setInt32(kKeyHeight,
        pDecShellContext->m_pVideoStreamhandler->m_videoHeight);

    // Create the decoder source
    pDecShellContext->mReaderSource = new VideoEditorVideoDecoderSource(
        decoderMetadata, pDecShellContext->mDecoderType,
        (void *)pDecShellContext);
    VIDEOEDITOR_CHECK(NULL != pDecShellContext->mReaderSource.get(),
        M4ERR_SF_DECODER_RSRC_FAIL);

    // Connect to the OMX client
    status = pDecShellContext->mClient.connect();
    VIDEOEDITOR_CHECK(OK == status, M4ERR_SF_DECODER_RSRC_FAIL);

    // Create the decoder
    pDecShellContext->mVideoDecoder = OMXCodec::Create(
        pDecShellContext->mClient.interface(),
        decoderMetadata, false, pDecShellContext->mReaderSource);
    VIDEOEDITOR_CHECK(NULL != pDecShellContext->mVideoDecoder.get(),
        M4ERR_SF_DECODER_RSRC_FAIL);


    // Get the output color format
    success = pDecShellContext->mVideoDecoder->getFormat()->findInt32(
        kKeyColorFormat, &colorFormat);
    VIDEOEDITOR_CHECK(TRUE == success, M4ERR_PARAMETER);
    pDecShellContext->decOuputColorFormat = (OMX_COLOR_FORMATTYPE)colorFormat;

    pDecShellContext->mVideoDecoder->getFormat()->setInt32(kKeyWidth,
        pDecShellContext->m_pVideoStreamhandler->m_videoWidth);
    pDecShellContext->mVideoDecoder->getFormat()->setInt32(kKeyHeight,
        pDecShellContext->m_pVideoStreamhandler->m_videoHeight);

    // Configure the buffer pool from the metadata
    err = VideoEditorVideoDecoder_configureFromMetadata(pDecShellContext,
        pDecShellContext->mVideoDecoder->getFormat().get());
    VIDEOEDITOR_CHECK(M4NO_ERROR == err, err);

    // Start the graph
    status = pDecShellContext->mVideoDecoder->start();
    VIDEOEDITOR_CHECK(OK == status, M4ERR_SF_DECODER_RSRC_FAIL);

    *pContext = (M4OSA_Context)pDecShellContext;

cleanUp:
    if( M4NO_ERROR == err ) {
        LOGV("VideoEditorVideoDecoder_create no error");
    } else {
        VideoEditorVideoDecoder_destroy(pDecShellContext);
        *pContext = M4OSA_NULL;
        LOGV("VideoEditorVideoDecoder_create ERROR 0x%X", err);
    }
    LOGV("VideoEditorVideoDecoder_create : DONE");
    return err;
}

M4OSA_ERR VideoEditorVideoDecoder_getOption(M4OSA_Context context,
        M4OSA_OptionID optionId, M4OSA_DataOption pValue) {
    M4OSA_ERR lerr = M4NO_ERROR;
    VideoEditorVideoDecoder_Context* pDecShellContext =
        (VideoEditorVideoDecoder_Context*) context;
    M4_VersionInfo* pVersionInfo;
    M4DECODER_VideoSize* pVideoSize;
    M4OSA_UInt32* pNextFrameCts;
    M4OSA_UInt32 *plastDecodedFrameCts;
    M4DECODER_AVCProfileLevel* profile;
    M4DECODER_MPEG4_DecoderConfigInfo* pDecConfInfo;

    LOGV("VideoEditorVideoDecoder_getOption begin");

    switch (optionId) {
        case M4DECODER_kOptionID_AVCLastDecodedFrameCTS:
             plastDecodedFrameCts = (M4OSA_UInt32 *) pValue;
             *plastDecodedFrameCts = pDecShellContext->m_lastDecodedCTS;
             break;

        case M4DECODER_kOptionID_Version:
            pVersionInfo = (M4_VersionInfo*)pValue;

            pVersionInfo->m_major = VIDEOEDITOR_VIDEC_SHELL_VER_MAJOR;
            pVersionInfo->m_minor= VIDEOEDITOR_VIDEC_SHELL_VER_MINOR;
            pVersionInfo->m_revision = VIDEOEDITOR_VIDEC_SHELL_VER_REVISION;
            pVersionInfo->m_structSize=sizeof(M4_VersionInfo);
            break;

        case M4DECODER_kOptionID_VideoSize:
            /** Only VPS uses this Option ID. */
            pVideoSize = (M4DECODER_VideoSize*)pValue;
            pDecShellContext->mVideoDecoder->getFormat()->findInt32(kKeyWidth,
                (int32_t*)(&pVideoSize->m_uiWidth));
            pDecShellContext->mVideoDecoder->getFormat()->findInt32(kKeyHeight,
                (int32_t*)(&pVideoSize->m_uiHeight));
            LOGV("VideoEditorVideoDecoder_getOption : W=%d H=%d",
                pVideoSize->m_uiWidth, pVideoSize->m_uiHeight);
            break;

        case M4DECODER_kOptionID_NextRenderedFrameCTS:
            /** How to get this information. SF decoder does not provide this. *
            ** Let us provide last decoded frame CTS as of now. *
            ** Only VPS uses this Option ID. */
            pNextFrameCts = (M4OSA_UInt32 *)pValue;
            *pNextFrameCts = pDecShellContext->m_lastDecodedCTS;
            break;
        case M4DECODER_kOptionID_AVCProfileAndLevel:
            profile = (M4DECODER_AVCProfileLevel *) pValue;
            VideoEditorVideoDecoder_ParseAVCDSI (
                pDecShellContext->m_pVideoStreamhandler->\
                    m_basicProperties.m_pDecoderSpecificInfo,
                pDecShellContext->m_pVideoStreamhandler->\
                    m_basicProperties.m_decoderSpecificInfoSize,
                profile);
            break;
        case M4DECODER_MPEG4_kOptionID_DecoderConfigInfo:
            if(pDecShellContext->mDecoderType == VIDEOEDITOR_kMpeg4VideoDec) {
                (*(M4DECODER_MPEG4_DecoderConfigInfo*)pValue) =
                    pDecShellContext->m_Dci;
            }
            break;
        default:
            lerr = M4ERR_BAD_OPTION_ID;
            break;

    }

    LOGV("VideoEditorVideoDecoder_getOption: end with err = 0x%x", lerr);
    return lerr;
}

M4OSA_ERR VideoEditorVideoDecoder_setOption(M4OSA_Context context,
        M4OSA_OptionID optionId, M4OSA_DataOption pValue) {
    M4OSA_ERR lerr = M4NO_ERROR;
    VideoEditorVideoDecoder_Context *pDecShellContext =
        (VideoEditorVideoDecoder_Context*) context;

    LOGV("VideoEditorVideoDecoder_setOption begin");

    switch (optionId) {
        case M4DECODER_kOptionID_OutputFilter: {
                M4DECODER_OutputFilter* pOutputFilter =
                    (M4DECODER_OutputFilter*) pValue;
                pDecShellContext->m_pFilter =
                    (M4VIFI_PlanConverterFunctionType*)pOutputFilter->\
                    m_pFilterFunction;
                pDecShellContext->m_pFilterUserData =
                    pOutputFilter->m_pFilterUserData;
            }
            break;
        case M4DECODER_kOptionID_DeblockingFilter:
            break;
        default:
            lerr = M4ERR_BAD_CONTEXT;
            break;
    }

    LOGV("VideoEditorVideoDecoder_setOption: end with err = 0x%x", lerr);
    return lerr;
}

M4OSA_ERR VideoEditorVideoDecoder_decode(M4OSA_Context context,
        M4_MediaTime* pTime, M4OSA_Bool bJump) {
    M4OSA_ERR lerr = M4NO_ERROR;
    VideoEditorVideoDecoder_Context* pDecShellContext =
        (VideoEditorVideoDecoder_Context*) context;
    int64_t lFrameTime;
    VIDEOEDITOR_BUFFER_Buffer* tmpDecBuffer;
    MediaSource::ReadOptions decShellOptions;
    MediaBuffer* pDecoderBuffer = NULL;
    status_t errStatus;


    LOGV("VideoEditorVideoDecoder_decode begin");

    if( M4OSA_TRUE == pDecShellContext->mReachedEOS ) {
        // Do not call read(), it could lead to a freeze
        LOGV("VideoEditorVideoDecoder_decode : EOS already reached");
        lerr = M4WAR_NO_MORE_AU;
        goto VIDEOEDITOR_VideoDecode_cleanUP;
    }
    if(pDecShellContext->m_lastDecodedCTS >= *pTime) {
        LOGV("VideoDecoder_decode: Already decoded up to this time CTS = %lf.",
            pDecShellContext->m_lastDecodedCTS);
        goto VIDEOEDITOR_VideoDecode_cleanUP;
    }
    if(M4OSA_TRUE == bJump) {
        LOGV("VideoEditorVideoDecoder_decode: Jump called");
        pDecShellContext->m_lastDecodedCTS = -1;
        pDecShellContext->m_lastRenderCts = -1;
    }

    pDecShellContext->mNbInputFrames++;
    if (0 > pDecShellContext->mFirstInputCts){
        pDecShellContext->mFirstInputCts = *pTime;
    }
    pDecShellContext->mLastInputCts = *pTime;

    while (pDecShellContext->m_lastDecodedCTS < *pTime) {
        LOGV("VideoEditorVideoDecoder_decode, frameCTS = %lf, DecodeUpTo = %lf",
            pDecShellContext->m_lastDecodedCTS, *pTime);
        lerr = VIDEOEDITOR_BUFFER_getBuffer(pDecShellContext->m_pDecBufferPool,
            VIDEOEDITOR_BUFFER_kEmpty, &tmpDecBuffer);
        if (lerr == (M4OSA_UInt32)M4ERR_NO_BUFFER_AVAILABLE) {
            lerr = VIDEOEDITOR_BUFFER_getOldestBuffer(
                pDecShellContext->m_pDecBufferPool,
                VIDEOEDITOR_BUFFER_kFilled, &tmpDecBuffer);
            tmpDecBuffer->state = VIDEOEDITOR_BUFFER_kEmpty;
            lerr = M4NO_ERROR;
        }

        if (lerr != M4NO_ERROR) {
            goto VIDEOEDITOR_VideoDecode_cleanUP;
        }

        if (pDecoderBuffer != NULL) {
            pDecoderBuffer->release();
            pDecoderBuffer = NULL;
        }

        decShellOptions.reset();
        errStatus = pDecShellContext->mVideoDecoder->read(&pDecoderBuffer,
            &decShellOptions);
        if (errStatus == ERROR_END_OF_STREAM) {
            LOGV("End of stream reached, returning M4WAR_NO_MORE_AU ");
            pDecShellContext->mReachedEOS = M4OSA_TRUE;
            lerr = M4WAR_NO_MORE_AU;
            goto VIDEOEDITOR_VideoDecode_cleanUP;
        } else if ( INFO_FORMAT_CHANGED == errStatus ) {
            LOGV("VideoDecoder_decode:source returns INFO_FORMAT_CHANGED:TODO");

#if 1
            LOGV("VideoDecoder_decode : source returns INFO_FORMAT_CHANGED");
            lerr = VideoEditorVideoDecoder_configureFromMetadata(
                pDecShellContext,
                pDecShellContext->mVideoDecoder->getFormat().get());
            if( M4NO_ERROR != lerr ) {
                LOGV("!!! VideoEditorVideoDecoder_decode ERROR : "
                    "VideoDecoder_configureFromMetadata returns 0x%X", lerr);
                break;
            }
#endif
            continue;
        }

        if( 0 < pDecoderBuffer->range_length() ) {
        LOGV("VIDEOEDITOR_VideoDecoder frame buffer size = %d",
            pDecoderBuffer->range_length());

        pDecoderBuffer->meta_data()->findInt64(kKeyTime, &lFrameTime);
        pDecShellContext->m_lastDecodedCTS = (M4_MediaTime)(lFrameTime/1000);
        LOGV("VideoEditorVideoDecoder_decode,decoded frametime = %lf,size = %d",
            (M4_MediaTime)lFrameTime, pDecoderBuffer->size() );

        switch ( pDecShellContext->decOuputColorFormat ) {
            case OMX_QCOM_COLOR_FormatYVU420SemiPlanar: {
                M4VIFI_ImagePlane tmpPlane[3];
                // Prepare the output image for conversion
                if( pDecoderBuffer->range_length() != (
                    pDecShellContext->m_pVideoStreamhandler->m_videoWidth *
                    pDecShellContext->m_pVideoStreamhandler->m_videoHeight \
                     * 3)/2 ) {
                    LOGV("VideoEditorVideoDecoder_decod invalid frame size S=%d"
                        "W=%d H=%d", pDecoderBuffer->range_length(),
                        pDecShellContext->m_pVideoStreamhandler->m_videoWidth,
                        pDecShellContext->m_pVideoStreamhandler->m_videoHeight);
                    lerr = M4ERR_PARAMETER;
                    goto VIDEOEDITOR_VideoDecode_cleanUP;
                }
                tmpPlane[0].u_width   =
                    pDecShellContext->m_pVideoStreamhandler->m_videoWidth;
                tmpPlane[0].u_height  =
                    pDecShellContext->m_pVideoStreamhandler->m_videoHeight;
                tmpPlane[0].u_topleft = 0;
                tmpPlane[0].u_stride  = tmpPlane[0].u_width;
                tmpPlane[0].pac_data  = (M4VIFI_UInt8*)tmpDecBuffer->pData;
                tmpPlane[1].u_width   = tmpPlane[0].u_width/2;
                tmpPlane[1].u_height  = tmpPlane[0].u_height/2;
                tmpPlane[1].u_topleft = 0;
                tmpPlane[1].u_stride  = tmpPlane[0].u_stride/2;
                tmpPlane[1].pac_data  = tmpPlane[0].pac_data +
                    (tmpPlane[0].u_stride * tmpPlane[0].u_height);
                tmpPlane[2].u_width   = tmpPlane[1].u_width;
                tmpPlane[2].u_height  = tmpPlane[1].u_height;
                tmpPlane[2].u_topleft = 0;
                tmpPlane[2].u_stride  = tmpPlane[1].u_stride;
                tmpPlane[2].pac_data  = tmpPlane[1].pac_data +
                    (tmpPlane[1].u_stride * tmpPlane[1].u_height);
                M4VIFI_SemiplanarYVU420toYUV420(M4OSA_NULL,
                    (M4VIFI_UInt8 *)pDecoderBuffer->data() + \
                    pDecoderBuffer->range_offset(), &tmpPlane[0]);
                break;
            }
            case OMX_COLOR_FormatYUV420Planar:
                M4OSA_memcpy((M4OSA_MemAddr8)tmpDecBuffer->pData,
                    (M4OSA_MemAddr8) pDecoderBuffer->data() +
                    pDecoderBuffer->range_offset(),
                    (M4OSA_UInt32)pDecoderBuffer->range_length());
                break;
            default:
                LOGV("VideoDecoder_decode: unexpected color format 0x%X",
                    pDecShellContext->decOuputColorFormat);
                return M4ERR_PARAMETER;
        }

        tmpDecBuffer->buffCTS = pDecShellContext->m_lastDecodedCTS;
        tmpDecBuffer->state = VIDEOEDITOR_BUFFER_kFilled;
        tmpDecBuffer->size = pDecoderBuffer->size();

        } else {
            LOGV("VideoEditorVideoDecoder_decode : empty buffer was returned");
        }
    }
    pDecShellContext->mNbOutputFrames++;
    if ( 0 > pDecShellContext->mFirstOutputCts ) {
        pDecShellContext->mFirstOutputCts = *pTime;
    }
    pDecShellContext->mLastOutputCts = *pTime;

VIDEOEDITOR_VideoDecode_cleanUP:
    *pTime = pDecShellContext->m_lastDecodedCTS;
    if (pDecoderBuffer != NULL) {
        pDecoderBuffer->release();
        pDecoderBuffer = NULL;
    }

    LOGV("VideoEditorVideoDecoder_decode: end with 0x%x", lerr);
    return lerr;
}

M4OSA_ERR VideoEditorVideoDecoder_render(M4OSA_Context context,
        M4_MediaTime* pTime, M4VIFI_ImagePlane* pOutputPlane,
        M4OSA_Bool bForceRender) {
    M4OSA_ERR err = M4NO_ERROR;
    VideoEditorVideoDecoder_Context* pDecShellContext =
        (VideoEditorVideoDecoder_Context*) context;
    M4OSA_UInt32 lindex, i;
    M4OSA_UInt8* p_buf_src, *p_buf_dest;
    M4VIFI_ImagePlane tmpPlaneIn, tmpPlaneOut;
    VIDEOEDITOR_BUFFER_Buffer* pTmpVIDEOEDITORBuffer, *pRenderVIDEOEDITORBuffer
                                                                  = M4OSA_NULL;
    M4_MediaTime candidateTimeStamp = -1;
    M4OSA_Bool bFound = M4OSA_FALSE;

    LOGV("VideoEditorVideoDecoder_render begin");
    // Input parameters check
    VIDEOEDITOR_CHECK(M4OSA_NULL != context, M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK(M4OSA_NULL != pTime, M4ERR_PARAMETER);
    VIDEOEDITOR_CHECK(M4OSA_NULL != pOutputPlane, M4ERR_PARAMETER);

    // The output buffer is already allocated, just copy the data
    if ( (*pTime <= pDecShellContext->m_lastRenderCts) &&
            (M4OSA_FALSE == bForceRender) ) {
        LOGV("VIDEOEDITOR_VIDEO_render Frame in the past");
        err = M4WAR_VIDEORENDERER_NO_NEW_FRAME;
        goto cleanUp;
    }
    LOGV("VideoDecoder_render: lastRendered time = %lf,requested render time = "
        "%lf", pDecShellContext->m_lastRenderCts, *pTime);

    /**
     * Find the buffer appropriate for rendering.  */
    for (i=0; i < pDecShellContext->m_pDecBufferPool->NB; i++) {
        pTmpVIDEOEDITORBuffer = &pDecShellContext->m_pDecBufferPool\
            ->pNXPBuffer[i];
        if (pTmpVIDEOEDITORBuffer->state == VIDEOEDITOR_BUFFER_kFilled) {
            /** Free all those buffers older than last rendered frame. */
            if (pTmpVIDEOEDITORBuffer->buffCTS < pDecShellContext->\
                    m_lastRenderCts) {
                pTmpVIDEOEDITORBuffer->state = VIDEOEDITOR_BUFFER_kEmpty;
            }

            /** Get the buffer with appropriate timestamp  */
            if ( (pTmpVIDEOEDITORBuffer->buffCTS >= pDecShellContext->\
                    m_lastRenderCts) &&
                (pTmpVIDEOEDITORBuffer->buffCTS <= *pTime) &&
                (pTmpVIDEOEDITORBuffer->buffCTS > candidateTimeStamp)) {
                bFound = M4OSA_TRUE;
                pRenderVIDEOEDITORBuffer = pTmpVIDEOEDITORBuffer;
                candidateTimeStamp = pTmpVIDEOEDITORBuffer->buffCTS;
                LOGV("VideoDecoder_render: found a buffer with timestamp = %lf",
                    candidateTimeStamp);
            }
        }
    }
    if (M4OSA_FALSE == bFound) {
        err = M4WAR_VIDEORENDERER_NO_NEW_FRAME;
        goto cleanUp;
    }

    LOGV("VideoEditorVideoDecoder_render 3 ouput %d %d %d %d",
        pOutputPlane[0].u_width, pOutputPlane[0].u_height,
        pOutputPlane[0].u_topleft, pOutputPlane[0].u_stride);

    pDecShellContext->m_lastRenderCts = candidateTimeStamp;

    if( M4OSA_NULL != pDecShellContext->m_pFilter ) {
        // Filtering was requested
        M4VIFI_ImagePlane tmpPlane[3];
        // Prepare the output image for conversion
        tmpPlane[0].u_width   =
            pDecShellContext->m_pVideoStreamhandler->m_videoWidth;
        tmpPlane[0].u_height  =
            pDecShellContext->m_pVideoStreamhandler->m_videoHeight;
        tmpPlane[0].u_topleft = 0;
        tmpPlane[0].u_stride  = tmpPlane[0].u_width;
        tmpPlane[0].pac_data  = (M4VIFI_UInt8*)pRenderVIDEOEDITORBuffer->pData;
        tmpPlane[1].u_width   = tmpPlane[0].u_width/2;
        tmpPlane[1].u_height  = tmpPlane[0].u_height/2;
        tmpPlane[1].u_topleft = 0;
        tmpPlane[1].u_stride  = tmpPlane[0].u_stride/2;
        tmpPlane[1].pac_data  = tmpPlane[0].pac_data +
            (tmpPlane[0].u_stride * tmpPlane[0].u_height);
        tmpPlane[2].u_width   = tmpPlane[1].u_width;
        tmpPlane[2].u_height  = tmpPlane[1].u_height;
        tmpPlane[2].u_topleft = 0;
        tmpPlane[2].u_stride  = tmpPlane[1].u_stride;
        tmpPlane[2].pac_data  = tmpPlane[1].pac_data +
            (tmpPlane[1].u_stride * tmpPlane[1].u_height);

        LOGV("VideoEditorVideoDecoder_render w = %d H = %d",
            tmpPlane[0].u_width,tmpPlane[0].u_height);
        pDecShellContext->m_pFilter(M4OSA_NULL, &tmpPlane[0], pOutputPlane);
    } else {
        // Just copy the YUV420P buffer
        M4OSA_MemAddr8 tempBuffPtr =
            (M4OSA_MemAddr8)pRenderVIDEOEDITORBuffer->pData;
        M4OSA_UInt32 tempWidth =
            pDecShellContext->m_pVideoStreamhandler->m_videoWidth;
        M4OSA_UInt32 tempHeight =
            pDecShellContext->m_pVideoStreamhandler->m_videoHeight;

        M4OSA_memcpy((M4OSA_MemAddr8) pOutputPlane[0].pac_data, tempBuffPtr,
            tempWidth * tempHeight);
        tempBuffPtr += (tempWidth * tempHeight);
        M4OSA_memcpy((M4OSA_MemAddr8) pOutputPlane[1].pac_data, tempBuffPtr,
            (tempWidth/2) * (tempHeight/2));
        tempBuffPtr += ((tempWidth/2) * (tempHeight/2));
        M4OSA_memcpy((M4OSA_MemAddr8) pOutputPlane[2].pac_data, tempBuffPtr,
            (tempWidth/2) * (tempHeight/2));
    }

    pDecShellContext->mNbRenderedFrames++;
    if ( 0 > pDecShellContext->mFirstRenderedCts ) {
        pDecShellContext->mFirstRenderedCts = *pTime;
    }
    pDecShellContext->mLastRenderedCts = *pTime;

cleanUp:
    if( M4NO_ERROR == err ) {
        *pTime = pDecShellContext->m_lastRenderCts;
        LOGV("VideoEditorVideoDecoder_render no error");
    } else {
        LOGV("VideoEditorVideoDecoder_render ERROR 0x%X", err);
    }
    LOGV("VideoEditorVideoDecoder_render end");
    return err;
}

M4OSA_ERR VideoEditorVideoDecoder_getInterface(M4DECODER_VideoType decoderType,
        M4DECODER_VideoType *pDecoderType, M4OSA_Context *pDecInterface) {
    M4DECODER_VideoInterface* pDecoderInterface = M4OSA_NULL;

    pDecoderInterface = (M4DECODER_VideoInterface*)M4OSA_malloc(
        sizeof(M4DECODER_VideoInterface), M4DECODER_EXTERNAL,
        (M4OSA_Char*)"VideoEditorVideoDecoder_getInterface" );
    if (M4OSA_NULL == pDecoderInterface) {
        return M4ERR_ALLOC;
    }

    *pDecoderType = decoderType;

    pDecoderInterface->m_pFctCreate    = VideoEditorVideoDecoder_create;
    pDecoderInterface->m_pFctDestroy   = VideoEditorVideoDecoder_destroy;
    pDecoderInterface->m_pFctGetOption = VideoEditorVideoDecoder_getOption;
    pDecoderInterface->m_pFctSetOption = VideoEditorVideoDecoder_setOption;
    pDecoderInterface->m_pFctDecode    = VideoEditorVideoDecoder_decode;
    pDecoderInterface->m_pFctRender    = VideoEditorVideoDecoder_render;

    *pDecInterface = (M4OSA_Context)pDecoderInterface;
    return M4NO_ERROR;
}

extern "C" {

M4OSA_ERR VideoEditorVideoDecoder_getInterface_MPEG4(
        M4DECODER_VideoType *pDecoderType, M4OSA_Context *pDecInterface) {
    return VideoEditorVideoDecoder_getInterface(M4DECODER_kVideoTypeMPEG4,
        pDecoderType, pDecInterface);
}

M4OSA_ERR VideoEditorVideoDecoder_getInterface_H264(
        M4DECODER_VideoType *pDecoderType, M4OSA_Context *pDecInterface) {
    return VideoEditorVideoDecoder_getInterface(M4DECODER_kVideoTypeAVC,
        pDecoderType, pDecInterface);

}

}  // extern "C"
