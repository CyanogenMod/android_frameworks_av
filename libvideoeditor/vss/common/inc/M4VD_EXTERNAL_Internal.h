/*
 * Copyright (C) 2004-2011 NXP Software
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

#ifndef __M4VD_EXTERNAL_INTERNAL_H__
#define __M4VD_EXTERNAL_INTERNAL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "NXPSW_CompilerSwitches.h"

#ifndef M4DECODER_EXTERNAL_SYNC_EXT_DECODE
#include "M4OSA_Semaphore.h"
#endif /* not M4DECODER_EXTERNAL_SYNC_EXT_DECODE */

/*typedef enum
{
    M4VS_THREAD_IS_IDLE = 0,
    M4VS_THREAD_IS_RUNNING = 1,
    M4VS_THREAD_IS_STOPPING = 2

} M4VS_ThreadState_t;*/


/* ----- internal VS context ----- */

typedef struct
{
    /* READER */
    /**< Reference to the reader data interface used to read access units */
    M4READER_DataInterface*           m_pReader;
    /**< Reference to the access unit used read and decode one frame (the AU could be passed by
    the user instead of reading it from inside the decoder) */
    M4_AccessUnit*                    m_pNextAccessUnitToDecode;
    /**< Flag to know if we decode just after a (read) jump */
    M4OSA_Bool                        m_bJump;
    M4_MediaTime                      m_nextAUCts;                /**< CTS of the AU above */

    /* DECODER */

    M4_MediaTime             m_DecodeUpToCts;        /**< Target Cts for the decode up to loop */
    M4_MediaTime             m_CurrentDecodeCts;     /**< Cts of the latest frame decoded */
    M4_MediaTime             m_PreviousDecodeCts;    /**< Cts of the previous frame decoded */
    M4OSA_UInt32             m_NbDecodedFrames;      /**< Number of frames decoded in the decode
                                                          up to loop (can be 0) */
    M4OSA_ERR                m_uiDecodeError;        /**< Error or warning code (from the VD
                                                          reader or decoder) returned to the
                                                          shell */
    M4OSA_Bool               m_bDataDecodePending;   /**< There is some data to decode */
    M4OSA_Bool               m_bIsWaitNextDecode;    /**< Do we need to wait for the anticipated
                                                          decoding to finish ? */

    /* RENDER */

    M4_MediaTime                 m_TargetRenderCts;        /**< Cts for the rendering step */
    M4_MediaTime                 m_CurrentRenderCts;       /**< Cts of the latest frame decoded */
    M4OSA_ERR                    m_uiRenderError;          /**< Error or warning code (from the
                                                                VD render) returned to the shell */
    M4OSA_Bool                   m_bForceRender;           /**< Force rendering even if 0 frames
                                                                are decoded (i.e. already
                                                                previously decoded) */
    M4OSA_Bool                   m_bDataRenderPending;     /**< There is some data to render */

    /* STREAM PARAMS */

    M4_VideoStreamHandler*            m_pVideoStreamhandler;    /**< reference to the video
                                                                     stream description passed by
                                                                     the user */
    M4VD_StreamInfo*                  m_pStreamInfo;
    M4DECODER_VideoSize                  m_VideoSize;
    M4DECODER_MPEG4_DecoderConfigInfo m_Dci;                  /**< Information collected from
                                                                   DSI parsing */
    M4VIFI_ImagePlane*                m_pOutputPlane;         /**< Pointer to YUV output planes */

    /* VD API */

    M4VD_Interface*                   m_VD_Interface;           /**< pointers to HW functions */
    M4VD_SignalingInterface           m_VD_SignalingInterface;  /**< pointers to Shell signaling
                                                                     functions */
    M4VD_Context                      m_VD_Context;             /**< pointer to the real hardware
                                                                     context */

    /* THREAD STUFF  */
#ifndef M4DECODER_EXTERNAL_SYNC_EXT_DECODE
    M4OSA_Context                      m_SemSync;
#endif /* not M4DECODER_EXTERNAL_SYNC_EXT_DECODE */
} M4VS_VideoDecoder_Context;


/* ----- bitstream parser ----- */
/*
typedef struct
{
    M4OSA_UInt32 stream_byte;
    M4OSA_UInt32 stream_index;
    M4OSA_MemAddr8 in;

} M4VS_Bitstream_ctxt;
*/
#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __M4VD_EXTERNAL_INTERNAL_H__ */
