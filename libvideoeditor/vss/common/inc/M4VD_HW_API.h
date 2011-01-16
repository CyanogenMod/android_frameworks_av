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

#ifndef __M4VD_HW_API_H__
#define __M4VD_HW_API_H__

#include "M4OSA_Types.h"
#include "M4OSA_OptionID.h"
#include "M4OSA_CoreID.h"
#include "M4OSA_Error.h"
#include "M4OSA_Memory.h" /* M4OSA_MemAddrN */

#include "M4VIFI_FiltersAPI.h"

/**
 ************************************************************************
 * @file   M4VD_HW_API.H
 * @brief
 * @note
 ************************************************************************
*/

#ifdef __cplusplus
extern "C" {
#endif


/* ----- Hardware decoder errors and warnings ----- */

#define M4ERR_VD_FATAL        M4OSA_ERR_CREATE(M4_ERR, M4VD_EXTERNAL, 0x0001)


/* ----- enum definitions ----- */

typedef enum
{
    M4VD_kOptionId_Dummy = 0

} M4VD_OptionID;

typedef enum
{
    M4VD_kMpeg4VideoDec,
    M4VD_kH263VideoDec,
    M4VD_kH264VideoDec,
    M4VD_kVideoType_NB /* must remain last */
} M4VD_VideoType;

typedef enum
{
    M4VD_kNone,
    M4VD_kYUV420,
    M4VD_kYUV422,
    M4VD_kYUYV422,
    M4VD_kRGB565,
    M4VD_kBGR565

} M4VD_OutputFormat;


/* ----- structure definitions ----- */

typedef struct
{
    M4OSA_MemAddr32 pBuffer;              /**< pointer to video buffer - 32 bits aligned    */
    M4OSA_UInt32  bufferSize;             /**< the size in bytes of the buffer            */

} M4VD_VideoBuffer;

typedef struct
{
    M4OSA_UInt32 aWidth;                        /**< Width of the Image        */
    M4OSA_UInt32 aHeight;                        /**< Height of the Image    */

} M4VD_ImageSize;

typedef struct
{
    M4OSA_MemAddr8 pBuffer;                        /**< Pointer to the decoder configuration */
    M4OSA_UInt32 aSize;                            /**< Size of the buffer */

} M4VD_DecoderConfig;

typedef struct
{
    M4VD_ImageSize        anImageSize;            /**<Size of the image*/
    M4VD_DecoderConfig    decoderConfiguration;    /**<configuration of the decoder*/

} M4VD_StreamInfo;


/* ----- callbacks prototypes ----- */

typedef M4OSA_ERR (M4VD_CB_signalDecoderOver_fct)( M4OSA_Void* signalTarget,
                                                    M4OSA_Double frameTime, M4OSA_ERR err);
typedef M4OSA_ERR (M4VD_CB_signalRenderOver_fct) ( M4OSA_Void* signalTarget,
                                                    M4OSA_Double frameTime, M4OSA_ERR err);

typedef struct
{
    M4OSA_Void*                        m_pSignalTarget;

    /* decoder callbacks that need to be raised by HW decoder functions */
    M4VD_CB_signalDecoderOver_fct*    m_pFctSignalDecoderOver;
    M4VD_CB_signalRenderOver_fct*     m_pFctSignalRenderOver;

} M4VD_SignalingInterface;


/* ----- Hardware decoder functions set ----- */

typedef void* M4VD_Context; /* Video Decoder context (for M4VD_HW_xxxx functions) */


/* common */
typedef M4OSA_ERR (M4VD_init_fct)          ( M4VD_Context*, M4VD_SignalingInterface* );
typedef M4OSA_ERR (M4VD_setOption_fct)     ( M4VD_Context, M4VD_OptionID, M4OSA_DataOption );
typedef M4OSA_ERR (M4VD_getOption_fct)     ( M4VD_Context, M4VD_OptionID, M4OSA_DataOption* );
typedef M4OSA_ERR (M4VD_openDecoder_fct) ( M4VD_Context, M4VD_VideoType, M4VD_StreamInfo*,
                                            M4VD_OutputFormat*, M4OSA_Void* );
typedef M4OSA_ERR (M4VD_stepDecode_fct)    ( M4VD_Context, M4VD_VideoBuffer*, M4OSA_Double );
typedef M4OSA_ERR (M4VD_stepRender_fct)    ( M4VD_Context, M4VIFI_ImagePlane*, M4OSA_Double );
typedef M4OSA_ERR (M4VD_closeDecoder_fct)( M4VD_Context );
typedef M4OSA_ERR (M4VD_cleanUp_fct)       ( M4VD_Context );
typedef M4OSA_ERR (M4VD_setOutputFilter_fct)( M4VD_Context, M4VIFI_PlanConverterFunctionType*,
                                                M4OSA_Void*);

typedef struct
{
    M4VD_init_fct*                m_pFctInitVideoDecoder;
    M4VD_setOption_fct*            m_pFctSetOption;
    M4VD_getOption_fct*            m_pFctGetOption;
    M4VD_openDecoder_fct*        m_pFctOpenDecoder;
    M4VD_stepDecode_fct*        m_pFctStepDecode;
    M4VD_stepRender_fct*        m_pFctStepRender;
    M4VD_closeDecoder_fct*        m_pFctClose;
    M4VD_cleanUp_fct*            m_pFctCleanUp;
    M4VD_setOutputFilter_fct*    m_pFctSetOutputFilter;
} M4VD_Interface;

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __M4VD_HW_API_H__ */
