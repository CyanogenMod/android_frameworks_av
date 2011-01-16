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

/**
 ******************************************************************************
 * @file    M4VD_EXTERNAL_Interface.c
 * @brief
 * @note
 ******************************************************************************
 */

#include "NXPSW_CompilerSwitches.h"

#include "M4OSA_CoreID.h"
#include "M4OSA_Types.h"
#include "M4OSA_Debug.h"

#ifndef M4DECODER_EXTERNAL_SYNC_EXT_DECODE
#include "M4OSA_Semaphore.h"
#endif /* not M4DECODER_EXTERNAL_SYNC_EXT_DECODE */

#include "M4VD_EXTERNAL_Interface.h"
#include "M4VD_EXTERNAL_Internal.h"

/* Warning: the decode thread has finished decoding all the frames */
#define M4WAR_DECODE_FINISHED                                M4OSA_ERR_CREATE(M4_WAR,\
                                                                 M4DECODER_EXTERNAL, 0x0001)
/* Warning: the render thread has finished rendering the frame */
#define M4WAR_RENDER_FINISHED                                M4OSA_ERR_CREATE(M4_WAR,\
                                                                 M4DECODER_EXTERNAL, 0x0002)

#define M4ERR_CHECK(x) if(M4NO_ERROR!=x) return x;
#define M4ERR_EXIT(x) do { err = x; goto exit_with_error; } while(0)


/* ----- shell API ----- */

static M4OSA_ERR M4DECODER_EXTERNAL_create(M4OSA_Context *pVS_Context,
                                             M4_StreamHandler *pStreamHandler,
                                             M4READER_DataInterface *pReaderDataInterface,
                                             M4_AccessUnit* pAccessUnit, M4OSA_Void* pUserData);
static M4OSA_ERR M4DECODER_EXTERNAL_destroy(M4OSA_Context pVS_Context);
static M4OSA_ERR M4DECODER_EXTERNAL_getOption(M4OSA_Context pVS_Context, M4OSA_OptionID optionId,
                                                M4OSA_DataOption* pValue);
static M4OSA_ERR M4DECODER_EXTERNAL_setOption(M4OSA_Context pVS_Context, M4OSA_OptionID optionId,
                                                 M4OSA_DataOption pValue);
static M4OSA_ERR M4DECODER_EXTERNAL_decode(M4OSA_Context pVS_Context, M4_MediaTime* pTime,
                                             M4OSA_Bool bJump);
static M4OSA_ERR M4DECODER_EXTERNAL_render(M4OSA_Context pVS_Context, M4_MediaTime* pTime,
                                             M4VIFI_ImagePlane* pOutputPlane,
                                             M4OSA_Bool bForceRender);

/* ----- Signaling functions ----- */

static M4OSA_ERR M4DECODER_EXTERNAL_signalDecoderOver(M4OSA_Context pVS_Context,
                                                        M4_MediaTime aTime, M4OSA_ERR aUserError);
static M4OSA_ERR M4DECODER_EXTERNAL_signalRenderOver(M4OSA_Context pVS_Context,
                                                     M4_MediaTime aTime, M4OSA_ERR aUserError);

/* ----- static internal functions ----- */

static M4OSA_ERR M4DECODER_EXTERNAL_Init(void** pVS_Context, M4VD_Interface* p_HWInterface,
                                         M4_StreamHandler *pStreamHandler);
static M4OSA_ERR M4DECODER_EXTERNAL_StreamDescriptionInit(M4VD_StreamInfo** ppStreamInfo,
                                                             M4_StreamHandler *pStreamHandler);
static M4OSA_ERR M4DECODER_EXTERNAL_SetUpReadInput(void* pVS_Context,
                                                     M4READER_DataInterface* pReader,
                                                     M4_AccessUnit* pAccessUnit);
static M4OSA_ERR M4DECODER_EXTERNAL_GetNextAu(M4VS_VideoDecoder_Context* pStreamContext,
                                                 M4VD_VideoBuffer *nextBuffer,
                                                 M4_MediaTime* nextFrameTime);
static M4OSA_ERR M4DECODER_EXTERNAL_SynchronousDecode(M4OSA_Context pVS_Context);
static M4OSA_ERR M4DECODER_EXTERNAL_AsynchronousDecode(M4OSA_Context pVS_Context);
static M4OSA_ERR M4DECODER_EXTERNAL_AsynchronousRender(M4OSA_Context pVS_Context);


/* ___________________________________________________________________ */
/*|                                                                   |*/
/*|                                                       |*/
/*|___________________________________________________________________|*/

/**
 ************************************************************************
 * @brief   Retrieves the interface implemented by the decoder
 * @note
 *
 * @param   pDecoderInterface: (OUT) address of a pointer that will be set to the interface
 *                                   implemented by this decoder. The interface is a structure
 *                                   allocated by the function and must be unallocated by the
 *                                   caller.
 *
 * @returns : M4NO_ERROR  if OK
 *            M4ERR_ALLOC if allocation failed
 ************************************************************************
 */
M4OSA_ERR M4DECODER_EXTERNAL_getInterface(M4DECODER_VideoInterface **pDecoderInterface)
{
    /* Allocates memory for the decoder shell pointer to function */
    *pDecoderInterface =
         (M4DECODER_VideoInterface*)M4OSA_malloc( sizeof(M4DECODER_VideoInterface),
             M4DECODER_EXTERNAL, (M4OSA_Char *)"M4DECODER_VideoInterface" );
    if (M4OSA_NULL == *pDecoderInterface)
    {
        M4OSA_TRACE1_0("M4DECODER_EXTERNAL_getInterface:\
             unable to allocate M4DECODER_VideoInterface, returning M4ERR_ALLOC");
        return M4ERR_ALLOC;
    }

    (*pDecoderInterface)->m_pFctCreate    = M4DECODER_EXTERNAL_create;
    (*pDecoderInterface)->m_pFctDestroy   = M4DECODER_EXTERNAL_destroy;
    (*pDecoderInterface)->m_pFctGetOption = M4DECODER_EXTERNAL_getOption;
    (*pDecoderInterface)->m_pFctSetOption = M4DECODER_EXTERNAL_setOption;
    (*pDecoderInterface)->m_pFctDecode    = M4DECODER_EXTERNAL_decode;
    (*pDecoderInterface)->m_pFctRender    = M4DECODER_EXTERNAL_render;

    return M4NO_ERROR;
}


/* ___________________________________________________________________ */
/*|                                                                   |*/
/*|                           shell API                            |*/
/*|___________________________________________________________________|*/

/**
 ************************************************************************
 * @brief   Creates the external video decoder
 * @note    This function creates internal video decoder context and
 *          initializes it.
 *
 * @param   pVS_Context     (OUT)   Context of the video hw shell
 * @param   pStreamHandler  (IN)    Pointer to a video stream description
 * @param   pReaderDataInterface: (IN)  Pointer to the M4READER_DataInterface
 *                                  structure that must be used by the
 *                                  decoder to read data from the stream
 * @param   pAccessUnit     (IN)    Pointer to an access unit (allocated
 *                                  by the caller) where the decoded data
 *                                  are stored
 * @param   pExternalAPI    (IN)    Interface of the client video decoder
 * @param   pUserData       (IN)    User data of the external video decoder
 *
 * @return  M4NO_ERROR              There is no error
 * @return  M4ERR_ALLOC             a memory allocation has failed
 * @return  M4ERR_PARAMETER         at least one parameter is not properly set (in DEBUG only)
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_create(M4OSA_Context *pVS_Context,
                                             M4_StreamHandler *pStreamHandler,
                                             M4READER_DataInterface *pReaderDataInterface,
                                             M4_AccessUnit* pAccessUnit, M4OSA_Void* pUserData)
{
    M4VD_VideoType videoDecoderKind;
    M4VD_StreamInfo* pStreamInfo;
    M4VD_OutputFormat outputFormat;

    M4VS_VideoDecoder_Context* pStreamContext;
    M4OSA_ERR err = M4NO_ERROR;

    M4OSA_TRACE2_0("M4DECODER_EXTERNAL_create");

    /* Video Shell Creation */
    err = M4DECODER_EXTERNAL_Init(pVS_Context,
         ((M4DECODER_EXTERNAL_UserDataType)pUserData)->externalFuncs, pStreamHandler);

    if (err != M4NO_ERROR)
    {
        M4OSA_TRACE1_1("M4DECODER_EXTERNAL_create :\
             M4VD_EXTERNAL_Init RETURNS THE ERROR CODE = 0x%x", err);
        return err;
    }

    err = M4DECODER_EXTERNAL_SetUpReadInput(*pVS_Context, pReaderDataInterface, pAccessUnit);

    if (err != M4NO_ERROR)
    {
        M4OSA_TRACE1_1("M4DECODER_EXTERNAL_create :\
             M4VD_EXTERNAL_SetUpReadInput RETURNS THE ERROR CODE = 0x%x", err);
        return err;
    }

    pStreamContext = (M4VS_VideoDecoder_Context*)(*pVS_Context);

    /* Stream Description init */
    err = M4DECODER_EXTERNAL_StreamDescriptionInit(&pStreamInfo, pStreamHandler);

    if (err != M4NO_ERROR)
    {
        M4OSA_TRACE1_1("M4DECODER_EXTERNAL_create :\
             M4VD_EXTERNAL_StreamDescriptionInit RETURNS THE ERROR CODE = 0x%x", err);
        return err;
    }

    pStreamContext->m_pStreamInfo = pStreamInfo;

    /* HW context creation */
    err = pStreamContext->m_VD_Interface->m_pFctInitVideoDecoder(&(pStreamContext->m_VD_Context),
         &(pStreamContext->m_VD_SignalingInterface));

    if (err != M4NO_ERROR)
    {
        M4OSA_TRACE1_1("M4DECODER_EXTERNAL_create : m_pFctInitVideoDecoder() error 0x%x", err);
        return err;
    }

    /* HW decoder creation */
    switch(pStreamHandler->m_streamType)
    {
        case M4DA_StreamTypeVideoH263 :
            videoDecoderKind = M4VD_kH263VideoDec;
            break;

        default :
        case M4DA_StreamTypeVideoMpeg4 :
            videoDecoderKind = M4VD_kMpeg4VideoDec;
            break;
    }

    err = pStreamContext->m_VD_Interface->m_pFctOpenDecoder(pStreamContext->m_VD_Context,
         videoDecoderKind, pStreamContext->m_pStreamInfo, &outputFormat,
             ((M4DECODER_EXTERNAL_UserDataType)pUserData)->externalUserData);

    if (err != M4NO_ERROR)
    {
        M4OSA_TRACE1_1("M4DECODER_EXTERNAL_create : m_pFctOpenDecoder() error 0x%x", err);
        return err;
    }

    /* Parse the VOL header */
    err = M4DECODER_EXTERNAL_ParseVideoDSI((M4OSA_UInt8 *)pStreamContext->m_pStreamInfo->\
                                           decoderConfiguration.pBuffer,
                                           pStreamContext->m_pStreamInfo->\
                                           decoderConfiguration.aSize,
                                           &pStreamContext->m_Dci, &pStreamContext->m_VideoSize);

    if (err != M4NO_ERROR)
    {
        M4OSA_TRACE1_1("M4DECODER_EXTERNAL_create :\
             M4DECODER_EXTERNAL_ParseVideoDSI() error 0x%x", err);
        return err;
    }

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief   destroy the instance of the decoder
 * @note    after this call the context is invalid
 *
 * @param   pVS_Context:   (IN) Context of the decoder
 *
 * @return  M4NO_ERROR          There is no error
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_destroy(M4OSA_Context pVS_Context)
{
    M4VS_VideoDecoder_Context* pStreamContext = (M4VS_VideoDecoder_Context*)pVS_Context;

    M4OSA_TRACE2_0("M4DECODER_EXTERNAL_destroy");

    if(M4OSA_NULL != pStreamContext)
    {
        /* Call external API destroy function */
        pStreamContext->m_VD_Interface->m_pFctClose(pStreamContext->m_VD_Context);

        /* Destroy context */
        pStreamContext->m_VD_Interface->m_pFctCleanUp(pStreamContext->m_VD_Context);

        if(M4OSA_NULL != pStreamContext->m_pStreamInfo)
        {
            M4OSA_free((M4OSA_MemAddr32)pStreamContext->m_pStreamInfo);
            pStreamContext->m_pStreamInfo = M4OSA_NULL;
        }

#ifndef M4DECODER_EXTERNAL_SYNC_EXT_DECODE
        if (M4OSA_NULL != pStreamContext->m_SemSync)
        {
            M4OSA_semaphoreClose(pStreamContext->m_SemSync);
        }
#endif /* not M4DECODER_EXTERNAL_SYNC_EXT_DECODE */

        M4OSA_free((M4OSA_MemAddr32)pStreamContext);
        pStreamContext = M4OSA_NULL;
    }

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief   Get an option value from the decoder
 * @note    It allows the caller to retrieve a property value:
 *          - the size (width x height) of the image
 *          - the DSI properties
 *
 * @param   pVS_Context: (IN)       Context of the decoder
 * @param   optionId:    (IN)       indicates the option to set
 * @param   pValue:      (IN/OUT)   pointer to structure or value (allocated by user) where option
 *                                    is stored
 * @return  M4NO_ERROR              there is no error
 * @return  M4ERR_PARAMETER         The context is invalid (in DEBUG only)
 * @return  M4ERR_BAD_OPTION_ID     when the option ID is not a valid one
 * @return  M4ERR_STATE             State automaton is not applied
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_getOption(M4OSA_Context pVS_Context, M4OSA_OptionID optionId,
                                             M4OSA_DataOption *pValue)
{
    M4VS_VideoDecoder_Context* pStreamContext = (M4VS_VideoDecoder_Context*)pVS_Context;
    M4OSA_ERR err = M4NO_ERROR;

    M4OSA_TRACE2_0("M4DECODER_EXTERNAL_getOption");

    switch (optionId)
    {
        case M4DECODER_kOptionID_VideoSize:
            *((M4DECODER_VideoSize*)pValue) = pStreamContext->m_VideoSize;
            err = M4NO_ERROR;
            break;

        case M4DECODER_MPEG4_kOptionID_DecoderConfigInfo:
            *((M4DECODER_MPEG4_DecoderConfigInfo*)pValue) = pStreamContext->m_Dci;
            err = M4NO_ERROR;
            break;

        default:
            err = pStreamContext->m_VD_Interface->m_pFctGetOption(pStreamContext->m_VD_Context,
                     optionId, pValue);
            break;
    }

    return err;
}

/**
 ************************************************************************
 * @brief   set en option value of the decoder
 * @note    It allows the caller to set a property value:
 *          - Nothing implemented at this time
 *
 * @param   pVS_Context: (IN)       Context of the external video decoder shell
 * @param   optionId:    (IN)       Identifier indicating the option to set
 * @param   pValue:      (IN)       Pointer to structure or value (allocated by user) where
 *                                    option is stored
 * @return  M4NO_ERROR              There is no error
 * @return  M4ERR_BAD_OPTION_ID     The option ID is not a valid one
 * @return  M4ERR_STATE             State automaton is not applied
 * @return  M4ERR_PARAMETER         The option parameter is invalid
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_setOption(M4OSA_Context pVS_Context, M4OSA_OptionID optionId,
                                              M4OSA_DataOption pValue)
{
    M4VS_VideoDecoder_Context* pStreamContext = (M4VS_VideoDecoder_Context*)pVS_Context;
    M4OSA_ERR err;
    M4OSA_TRACE2_0("M4DECODER_EXTERNAL_setOption");

    switch (optionId)
    {
        case M4DECODER_kOptionID_OutputFilter:
        {
            M4DECODER_OutputFilter* pOutputFilter = (M4DECODER_OutputFilter*) pValue;
            err =
                pStreamContext->m_VD_Interface->m_pFctSetOutputFilter(pStreamContext->m_VD_Context,
                            (M4VIFI_PlanConverterFunctionType*)pOutputFilter->m_pFilterFunction,
                            pOutputFilter->m_pFilterUserData);
        }
        break;

        case M4DECODER_kOptionID_DeblockingFilter:
            err = M4NO_ERROR;
        break;

        default:
            err = pStreamContext->m_VD_Interface->m_pFctSetOption(pStreamContext->m_VD_Context,
                 optionId, pValue);
        break;
    }

    return err;
}

/**
 ************************************************************************
 * @brief   Decode video Access Units up to a target time
 * @note    Parse and decode the video until it can output a decoded image for which
 *          the composition time is equal or greater to the passed targeted time
 *          The data are read from the reader data interface passed to M4DECODER_EXTERNAL_create.
 *          If threaded mode, waits until previous decoding is over,
 *          and fill decoding parameters used by the decoding thread.
 *
 * @param   pVS_Context:(IN)        Context of the external video decoder shell
 * @param   pTime:      (IN/OUT)    IN: Time to decode up to (in milli secondes)
 *                                  OUT:Time of the last decoded frame (in ms)
 * @param   bJump:      (IN)        0 if no jump occured just before this call
 *                                  1 if a a jump has just been made
 *
 * @return  M4NO_ERROR              there is no error
 * @return  M4ERR_PARAMETER         at least one parameter is not properly set
 * @return  M4WAR_NO_MORE_AU        there is no more access unit to decode (end of stream)
 * @return  M4WAR_VIDEORENDERER_NO_NEW_FRAME    No frame to render
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_decode(M4OSA_Context pVS_Context, M4_MediaTime* pTime,
                                             M4OSA_Bool bJump)
{
    M4VS_VideoDecoder_Context* pStreamContext = (M4VS_VideoDecoder_Context*)pVS_Context;

    M4OSA_ERR err = M4NO_ERROR;

    M4OSA_TRACE2_2("M4DECODER_EXTERNAL_decode : up to %lf  bjump = 0x%x", *pTime, bJump);

    pStreamContext->m_DecodeUpToCts = *pTime;
    pStreamContext->m_bJump = bJump;
    if (bJump)
    {
        pStreamContext->m_CurrentDecodeCts = -1.0;
        pStreamContext->m_CurrentRenderCts = -1.0;
    }

    if(pStreamContext->m_DecodeUpToCts < pStreamContext->m_nextAUCts &&
        pStreamContext->m_CurrentRenderCts > pStreamContext->m_DecodeUpToCts)
    {
        /* It means that we do not need to launch another predecode, as we will reuse
             the previously decoded frame*/
        /* a warning is returned to the service to warn it about that .*/
        /* In that case, the service MUST NOT call render function, and must keep the
             previous frame */
        /* if necessary (i.e force render case)*/
        M4OSA_TRACE2_0("No decode is needed, same frame reused");
        return M4WAR_VIDEORENDERER_NO_NEW_FRAME;
    }

    /* If render has not been called for frame n, it means that n+1 frame decoding has
         not been launched
    -> do not wait for its decoding completion ...*/
    if(pStreamContext->m_bIsWaitNextDecode == M4OSA_TRUE)
    {
        /* wait for decode n+1 to complete */
        //M4semvalue--;
        //printf("Semaphore wait: %d\n", M4semvalue);
        pStreamContext->m_bIsWaitNextDecode = M4OSA_FALSE;
        M4OSA_semaphoreWait(pStreamContext->m_SemSync, M4OSA_WAIT_FOREVER);
    }
    if(pStreamContext->m_CurrentDecodeCts >= *pTime)
    {
        /* If we are not in this condition, it means that we ask for a frame after the
             "predecoded" frame */
        *pTime = pStreamContext->m_CurrentDecodeCts;
        return M4NO_ERROR;
    }

    pStreamContext->m_NbDecodedFrames = 0;
    pStreamContext->m_uiDecodeError = M4NO_ERROR;
    pStreamContext->m_bDataDecodePending = M4OSA_TRUE;
    pStreamContext->m_uiDecodeError = M4NO_ERROR;

    /* Launch DecodeUpTo process in synchronous mode */
    while(pStreamContext->m_uiDecodeError == M4NO_ERROR)
    {
        M4DECODER_EXTERNAL_SynchronousDecode(pVS_Context);
        /* return code is ignored, it is used only in M4OSA_Thread api */
    }

    *pTime = pStreamContext->m_CurrentDecodeCts;

    if ( (M4WAR_DECODE_FINISHED == pStreamContext->m_uiDecodeError)
        || (M4WAR_VIDEORENDERER_NO_NEW_FRAME == pStreamContext->m_uiDecodeError) )
    {
        pStreamContext->m_uiDecodeError = M4NO_ERROR;
    }

    return pStreamContext->m_uiDecodeError;
}

/**
 ************************************************************************
 * @brief   Renders the video at the specified time.
 * @note    If threaded mode, this function unlock the decoding thread,
 *          which also call the external rendering function.
 *          Else, just call external rendering function, and waits for its
 *          completion.
 *
 * @param   pVS_Context: (IN)       Context of the video decoder shell
 * @param   pTime:       (IN/OUT)   IN: Time to render to (in milli secondes)
 *                                  OUT:Time of the effectively rendered frame (in ms)
 * @param   pOutputPlane:(OUT)      Output plane filled with decoded data (converted)
 *                                  If NULL, the rendering is made by the external
 *                                  component.
 * @param   bForceRender:(IN)       1 if the image must be rendered even it has already been
 *                                  0 if not (in which case the function can return
 *                                    M4WAR_VIDEORENDERER_NO_NEW_FRAME)
 * @return  M4NO_ERROR              There is no error
 * @return  M4ERR_PARAMETER         At least one parameter is not properly set
 * @return  M4ERR_STATE             State automaton is not applied
 * @return  M4ERR_ALLOC             There is no more available memory
 * @return  M4WAR_VIDEORENDERER_NO_NEW_FRAME    If the frame to render has already been rendered
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_render(M4OSA_Context pVS_Context, M4_MediaTime* pTime,
                                           M4VIFI_ImagePlane* pOutputPlane,
                                           M4OSA_Bool bForceRender)
{
    M4OSA_ERR err = M4NO_ERROR;
    M4VS_VideoDecoder_Context* pStreamContext = (M4VS_VideoDecoder_Context*)pVS_Context;

    M4OSA_TRACE2_2("M4DECODER_EXTERNAL_render : pTime = %lf, forceRender: %d ", *pTime,
         bForceRender);

    pStreamContext->m_TargetRenderCts = *pTime;
    pStreamContext->m_pOutputPlane = pOutputPlane;
    pStreamContext->m_bForceRender = bForceRender;
    pStreamContext->m_uiRenderError = M4NO_ERROR;
    pStreamContext->m_bDataRenderPending = M4OSA_TRUE;

    /* Launch Render process in synchronous mode */
    while(pStreamContext->m_uiRenderError == M4NO_ERROR)
    {
        M4DECODER_EXTERNAL_AsynchronousRender(pVS_Context);
        /* return code is ignored, it is used only in M4OSA_Thread */
    }


    *pTime = pStreamContext->m_CurrentRenderCts;


    if (M4WAR_RENDER_FINISHED == pStreamContext->m_uiRenderError)
    {
        pStreamContext->m_uiRenderError = M4NO_ERROR;
    }

    return pStreamContext->m_uiRenderError;
}


/* ___________________________________________________________________ */
/*|                                                                   |*/
/*|                        Signaling functions                        |*/
/*|___________________________________________________________________|*/

/**
 ************************************************************************
 * @brief   Called by the HW video decoder to signal that a decoding is
 *          over
 * @note    The function gets another AU in the internal AU buffer, and
 *          launches the decoding.
 *          If no more AU are available, the M4DECODER_EXTERNAL_decode
 *          (or M4DECODER_EXTERNAL_render if threaded) function is unlocked
 *
 * @param   pVS_Context:    (IN)    context of the video hw shell
 * @param   aTime:          (IN)    time of the decoded frame
 * @param   aUserError      (IN)    error code returned to the VPS
 *
 * @return  M4NO_ERROR              There is no error
 * @return  M4ERR_HW_DECODER_xxx    A fatal error occured
 * @return  M4ERR_PARAMETER         At least one parameter is NULL
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_signalDecoderOver(M4OSA_Context pVS_Context,
                                                      M4_MediaTime aTime, M4OSA_ERR aUserError)
{
    M4VS_VideoDecoder_Context* pStreamContext = (M4VS_VideoDecoder_Context*)pVS_Context;

    M4OSA_TRACE2_1("M4DECODER_EXTERNAL_signalDecoderOver : aTime = %lf", aTime);

    pStreamContext->m_NbDecodedFrames++;
    pStreamContext->m_uiDecodeError = aUserError;
    pStreamContext->m_CurrentDecodeCts = aTime;

#ifndef M4DECODER_EXTERNAL_SYNC_EXT_DECODE
    /* give control back to stepDecode */
    //M4semvalue++;
    //printf("Semaphore post: %d\n", M4semvalue);
    M4OSA_semaphorePost(pStreamContext->m_SemSync);
#endif /* not M4DECODER_EXTERNAL_SYNC_EXT_DECODE */

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief   Called by the HW video renderer to signal that a rendering is
 *          over
 * @note    The function just post a semaphore to unblock
 *          M4DECODER_EXTERNAL_render function
 *
 * @param   pVS_Context:    (IN)    context of the video hw shell
 * @param   aTime:          (IN)    time of the decoded frame
 * @param   aUserError      (IN)    error code returned to the VPS
 *
 * @return  M4NO_ERROR              There is no error
 * @return  M4ERR_HW_DECODER_xxx    A fatal error occured
 * @return  M4ERR_PARAMETER         At least one parameter is NULL
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_signalRenderOver(M4OSA_Context pVS_Context,
                                                     M4_MediaTime aTime, M4OSA_ERR aUserError)
{
    M4VS_VideoDecoder_Context* pStreamContext = (M4VS_VideoDecoder_Context*)pVS_Context;

    M4OSA_TRACE3_1("M4DECODER_EXTERNAL_signalRenderOver : aTime = %lf", aTime);

    pStreamContext->m_uiRenderError = aUserError;
    pStreamContext->m_CurrentRenderCts = aTime;

#ifndef M4DECODER_EXTERNAL_SYNC_EXT_DECODE
    /* give control back to stepRender */
    //M4semvalue++;
    //printf("Semaphore post: %d\n", M4semvalue);
    M4OSA_semaphorePost(pStreamContext->m_SemSync);
#endif /* not M4DECODER_EXTERNAL_SYNC_EXT_DECODE */

    return M4NO_ERROR;
}


/* ___________________________________________________________________ */
/*|                                                                   |*/
/*|                            Internals                              |*/
/*|___________________________________________________________________|*/

/**
 ************************************************************************
 * @brief    Initializes the video decoder shell/handler
 * @note     allocates an execution context
 *
 * @param    pVS_Context:    (OUT)   Output context allocated
 * @param    p_HWInterface:  (IN)    Pointer on the set of external HW codec functions
 * @param    pStreamHandler: (IN)    Pointer to a video stream description
 *
 * @return   M4NO_ERROR     There is no error
 * @return   M4ERR_ALLOC    There is no more available memory
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_Init(M4OSA_Context* pVS_Context,
                                         M4VD_Interface* p_HWInterface,
                                         M4_StreamHandler *pStreamHandler)
{
    M4VS_VideoDecoder_Context* pStreamContext;

    M4OSA_ERR err = M4NO_ERROR;

    M4OSA_TRACE2_0("M4DECODER_EXTERNAL_Init");

    /* Allocate the internal context */
    *pVS_Context = M4OSA_NULL;

    pStreamContext = (M4VS_VideoDecoder_Context*)M4OSA_malloc(sizeof(M4VS_VideoDecoder_Context),
         M4DECODER_EXTERNAL,(M4OSA_Char *) "M4VS_VideoDecoder_Context");
    if (M4OSA_NULL == pStreamContext)
    {
        M4OSA_TRACE1_0("M4DECODER_EXTERNAL_Init : error, cannot allocate context !");
        return M4ERR_ALLOC;
    }

    /* Reset internal context structure */
    *pVS_Context = pStreamContext;

    /* --- READER --- */
    pStreamContext->m_pReader = M4OSA_NULL;
    pStreamContext->m_pNextAccessUnitToDecode = M4OSA_NULL;
    pStreamContext->m_bJump = M4OSA_FALSE;
    pStreamContext->m_nextAUCts = -1;

    /* --- DECODER --- */
    pStreamContext->m_DecodeUpToCts = -1;
    pStreamContext->m_CurrentDecodeCts = -1;
    pStreamContext->m_NbDecodedFrames = 0;
    pStreamContext->m_uiDecodeError = M4NO_ERROR;
    pStreamContext->m_bDataDecodePending = M4OSA_FALSE;
    pStreamContext->m_PreviousDecodeCts = 0;
    pStreamContext->m_bIsWaitNextDecode = M4OSA_FALSE;

    /* --- RENDER --- */
    pStreamContext->m_TargetRenderCts = -1;
    pStreamContext->m_CurrentRenderCts = -1;
    pStreamContext->m_uiRenderError = M4NO_ERROR;
    pStreamContext->m_bForceRender = M4OSA_TRUE;
    pStreamContext->m_bDataRenderPending = M4OSA_FALSE;

    /* --- STREAM PARAMS --- */
    pStreamContext->m_pVideoStreamhandler = (M4_VideoStreamHandler*)pStreamHandler;
    pStreamContext->m_pStreamInfo = M4OSA_NULL;
    pStreamContext->m_pOutputPlane = M4OSA_NULL;

    /* --- VD API --- */
    pStreamContext->m_VD_Interface = p_HWInterface;
    pStreamContext->m_VD_Context = M4OSA_NULL;

    pStreamContext->m_VD_SignalingInterface.m_pSignalTarget = pStreamContext;
    pStreamContext->m_VD_SignalingInterface.m_pFctSignalDecoderOver =
         M4DECODER_EXTERNAL_signalDecoderOver;
    pStreamContext->m_VD_SignalingInterface.m_pFctSignalRenderOver =
         M4DECODER_EXTERNAL_signalRenderOver;

    /* --- THREAD STUFF --- */

#ifndef M4DECODER_EXTERNAL_SYNC_EXT_DECODE
    pStreamContext->m_SemSync = M4OSA_NULL;
    //M4semvalue=0;
    err = M4OSA_semaphoreOpen(&(pStreamContext->m_SemSync), 0);
    if (M4NO_ERROR != err)
    {
        M4OSA_TRACE1_1("M4DECODER_EXTERNAL_Init: can't open sync semaphore (err 0x%08X)", err);
        return err;
    }
#endif /* not M4DECODER_EXTERNAL_SYNC_EXT_DECODE */

    return err;
}

/**
 ************************************************************************
 * @brief   Fills the stream info structure
 * @note    This function is called at decoder's creation time,
 *          allocates and fills video info structure
 *
 * @param   ppStreamInfo    (OUT)   Video info structure
 * @param   pStreamHandler  (IN)    Pointer to a video stream description
 *
 * @return  M4ERR_ALLOC     Memory allocation error
 * @return  M4NO_ERROR      There is no error
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_StreamDescriptionInit(M4VD_StreamInfo** ppStreamInfo,
                                                          M4_StreamHandler *pStreamHandler)
{
    M4_VideoStreamHandler* pVideoStreamHandler  = M4OSA_NULL;

    M4OSA_TRACE2_0("M4DECODER_EXTERNAL_StreamDescriptionInit");

    pVideoStreamHandler = (M4_VideoStreamHandler*)pStreamHandler;

    /* M4VD_StreamInfo allocation */
    *ppStreamInfo = (M4VD_StreamInfo*)M4OSA_malloc(sizeof(M4VD_StreamInfo),
         M4DECODER_EXTERNAL, (M4OSA_Char *)"M4VD_StreamInfo");
    if(M4OSA_NULL == *ppStreamInfo)
    {
        return M4ERR_ALLOC;
    }

    /* init values */
    (*ppStreamInfo)->anImageSize.aWidth  = pVideoStreamHandler->m_videoWidth;
    (*ppStreamInfo)->anImageSize.aHeight = pVideoStreamHandler->m_videoHeight;

    (*ppStreamInfo)->decoderConfiguration.pBuffer =
         (M4OSA_MemAddr8)pStreamHandler->m_pDecoderSpecificInfo;
    (*ppStreamInfo)->decoderConfiguration.aSize   = pStreamHandler->m_decoderSpecificInfoSize;

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief   Initializes current AU parameters
 * @note    It is called at decoder's creation time to initialize
 *          current decoder's AU.
 *
 * @param   pVS_Context (IN)    Context of the video decoder shell
 * @param   pReader     (IN)    Reader interface
 * @param   pAccessUnit (IN)    Access Unit structure used bu decoder
 *
 * @return
 * @return
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_SetUpReadInput(M4OSA_Context pVS_Context,
                                                    M4READER_DataInterface* pReader,
                                                    M4_AccessUnit* pAccessUnit)
{
    M4VS_VideoDecoder_Context* pStreamContext=(M4VS_VideoDecoder_Context*)pVS_Context;

    M4OSA_TRACE2_0("M4DECODER_EXTERNAL_SetUpReadInput");

    M4OSA_DEBUG_IF1((M4OSA_NULL == pStreamContext), M4ERR_PARAMETER,
         "M4DECODER_EXTERNAL_SetUpReadInput: invalid context pointer");
    M4OSA_DEBUG_IF1((M4OSA_NULL == pReader),        M4ERR_PARAMETER,
         "M4DECODER_EXTERNAL_SetUpReadInput: invalid pReader pointer");
    M4OSA_DEBUG_IF1((M4OSA_NULL == pAccessUnit),    M4ERR_PARAMETER,
         "M4DECODER_EXTERNAL_SetUpReadInput: invalid pAccessUnit pointer");

    pStreamContext->m_pReader = pReader;
    pStreamContext->m_pNextAccessUnitToDecode = pAccessUnit;

    pAccessUnit->m_streamID = 0;
    pAccessUnit->m_size = 0;
    pAccessUnit->m_CTS = 0;
    pAccessUnit->m_DTS = 0;
    pAccessUnit->m_attribute = 0;

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief   Gets the next AU from internal AU buffer
 * @note    This function is necessary to be able to have a decodeUpTo
 *          interface with the VPS.
 *          The AU are read from file by M4DECODER_EXTERNAL_decode function
 *          and stored into a buffer. This function is called internally
 *          to get these stored AU.
 *
 * @param   pStreamContext: (IN)        context of the video hw shell
 * @param   nextFrameTime:  (IN/OUT)    time of the AU
 *
 * @return  M4NO_ERROR          There is no error
 * @return  M4WAR_NO_MORE_AU    No more AU in internal buffer
 * @return  M4ERR_PARAMETER     One invalid parameter
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_GetNextAu(M4VS_VideoDecoder_Context* pStreamContext,
                                                 M4VD_VideoBuffer *nextBuffer,
                                                 M4_MediaTime* nextFrameTime)
{
    M4OSA_ERR err = M4NO_ERROR;
    M4_AccessUnit* pAccessUnit;

    M4OSA_TRACE3_0("M4DECODER_EXTERNAL_GetNextAu");

    /* Check context is valid */
    if(M4OSA_NULL == pStreamContext)
    {
        M4OSA_TRACE1_0("M4DECODER_EXTERNAL_GetNextAu : error pStreamContext is NULL");
        return M4ERR_PARAMETER;
    }

    /* Read the AU */
    pAccessUnit = pStreamContext->m_pNextAccessUnitToDecode;

    err = pStreamContext->m_pReader->m_pFctGetNextAu(pStreamContext->m_pReader->m_readerContext,
         (M4_StreamHandler*)pStreamContext->m_pVideoStreamhandler, pAccessUnit);

    if((err == M4WAR_NO_DATA_YET) || (err == M4WAR_NO_MORE_AU))
    {
        M4OSA_TRACE2_1("M4DECODER_EXTERNAL_GetNextAu : no data avalaible 0x%x", err);
    }
    else if(err != M4NO_ERROR)
    {
        M4OSA_TRACE1_1("M4DECODER_EXTERNAL_GetNextAu : filesystem error 0x%x", err);

        *nextFrameTime         = 0;
        nextBuffer->pBuffer    = M4OSA_NULL;
        nextBuffer->bufferSize = 0;

        return err;
    }

    /* Fill buffer */
    *nextFrameTime         = pAccessUnit->m_CTS;
    nextBuffer->pBuffer    = (M4OSA_MemAddr32)pAccessUnit->m_dataAddress;
    nextBuffer->bufferSize = pAccessUnit->m_size;

    M4OSA_TRACE3_1("M4DECODER_EXTERNAL_GetNextAu: AU obtained, time is %f", *nextFrameTime);

    return err;
}

/**
 ************************************************************************
 * @brief
 * @note
 *
 * @param    pVS_Context:    (IN)    Context of the video hw shell
 *
 * @return    M4NO_ERROR        There is no error
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_SynchronousDecode(M4OSA_Context pVS_Context)
{
    M4VS_VideoDecoder_Context* pStreamContext = (M4VS_VideoDecoder_Context*)pVS_Context;

    M4OSA_ERR err = M4NO_ERROR;
    M4VD_VideoBuffer nextBuffer;


    /* ----- decode process ----- */

    if(M4OSA_TRUE == pStreamContext->m_bDataDecodePending)
    {
        /* Targeted time is reached */
        if( pStreamContext->m_CurrentDecodeCts >= pStreamContext->m_DecodeUpToCts )
        {
            M4OSA_TRACE2_0("M4DECODER_EXTERNAL_SynchronousDecode :\
                 skip decode because synchronisation");

            if(pStreamContext->m_NbDecodedFrames > 0)
            {
                pStreamContext->m_uiDecodeError = M4WAR_DECODE_FINISHED;
            }
            else
            {
                pStreamContext->m_uiDecodeError = M4WAR_VIDEORENDERER_NO_NEW_FRAME;
            }

            M4ERR_EXIT(M4NO_ERROR);
        }

        pStreamContext->m_PreviousDecodeCts = pStreamContext->m_CurrentDecodeCts;

        /* Get the next AU */
        pStreamContext->m_uiDecodeError = M4DECODER_EXTERNAL_GetNextAu(pStreamContext,
             &nextBuffer, &pStreamContext->m_CurrentDecodeCts);

        if( M4NO_ERROR != pStreamContext->m_uiDecodeError )
        {
            if ( M4WAR_NO_MORE_AU != pStreamContext->m_uiDecodeError)
            {
                M4OSA_TRACE1_1("M4DECODER_EXTERNAL_SynchronousDecode :\
                     M4DECODER_EXTERNAL_GetNextAu error 0x%x", pStreamContext->m_uiDecodeError);
            }
            M4ERR_EXIT(pStreamContext->m_uiDecodeError);
        }

        /* Decode the AU */
        if(nextBuffer.bufferSize > 0)
        {
            pStreamContext->m_uiDecodeError =
                 pStreamContext->m_VD_Interface->m_pFctStepDecode(pStreamContext->m_VD_Context,
                     &nextBuffer, pStreamContext->m_CurrentDecodeCts);
#ifndef M4DECODER_EXTERNAL_SYNC_EXT_DECODE
            if ( (M4NO_ERROR == pStreamContext->m_uiDecodeError)
                /*|| (M4WAR_IO_PENDING == pStreamContext->m_uiDecodeError)*/ )
            {
                /* wait for decode to complete */
                //M4semvalue--;
                //printf("Semaphore wait 2: %d\n", M4semvalue);
                M4OSA_semaphoreWait(pStreamContext->m_SemSync, M4OSA_WAIT_FOREVER);
                /* by now the actual m_uiDecodeError has been set by signalDecode */
            }
#endif /* not M4DECODER_EXTERNAL_SYNC_EXT_DECODE */
            if(M4NO_ERROR != pStreamContext->m_uiDecodeError)
            {
                M4OSA_TRACE1_1("M4DECODER_EXTERNAL_SynchronousDecode : HW decoder error 0x%x",
                     pStreamContext->m_uiDecodeError);
                M4ERR_EXIT(M4NO_ERROR);
            }
        }
        else
        {
            M4ERR_EXIT(M4NO_ERROR);
        }
    }

    return M4NO_ERROR;


/* ----- Release resources if an error occured */
exit_with_error:

    /* Abort decoding */
    pStreamContext->m_bDataDecodePending = M4OSA_FALSE;

    if((M4NO_ERROR == pStreamContext->m_uiDecodeError) && (M4NO_ERROR != err))
    {
        pStreamContext->m_uiDecodeError = err;
    }

    return err;
}

/**
 ************************************************************************
 * @brief
 * @note
 *
 * @param    pVS_Context:    (IN)    Context of the video hw shell
 *
 * @return    M4NO_ERROR        There is no error
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_AsynchronousDecode(M4OSA_Context pVS_Context)
{
    M4VS_VideoDecoder_Context* pStreamContext = (M4VS_VideoDecoder_Context*)pVS_Context;

    M4OSA_ERR err = M4NO_ERROR;
    M4VD_VideoBuffer nextBuffer;


    /* ----- decode process ----- */

    if(M4OSA_TRUE == pStreamContext->m_bDataDecodePending)
    {
        pStreamContext->m_PreviousDecodeCts = pStreamContext->m_CurrentDecodeCts;

        /* Get the next AU */
        pStreamContext->m_uiDecodeError = M4DECODER_EXTERNAL_GetNextAu(pStreamContext,
             &nextBuffer, &pStreamContext->m_nextAUCts);

        if( M4NO_ERROR != pStreamContext->m_uiDecodeError )
        {
            if ( M4WAR_NO_MORE_AU != pStreamContext->m_uiDecodeError)
            {
                M4OSA_TRACE1_1("M4DECODER_EXTERNAL_AsynchronousDecode :\
                     M4DECODER_EXTERNAL_GetNextAu error 0x%x", pStreamContext->m_uiDecodeError);
            }
            //M4semvalue++;
            //printf("Semaphore post: %d\n", M4semvalue);
            //M4OSA_semaphorePost(pStreamContext->m_SemSync);
            M4ERR_EXIT(pStreamContext->m_uiDecodeError);
        }

        /* Decode the AU if needed */
        if(nextBuffer.bufferSize > 0)
        {
            pStreamContext->m_uiDecodeError =
                 pStreamContext->m_VD_Interface->m_pFctStepDecode(pStreamContext->m_VD_Context,
                    &nextBuffer, pStreamContext->m_nextAUCts\
                        /*pStreamContext->m_CurrentDecodeCts*/);
            if(M4NO_ERROR != pStreamContext->m_uiDecodeError)
            {
                M4OSA_TRACE1_1("M4DECODER_EXTERNAL_AsynchronousDecode : HW decoder error 0x%x",
                     pStreamContext->m_uiDecodeError);
                M4ERR_EXIT(M4NO_ERROR);
            }
            pStreamContext->m_bIsWaitNextDecode = M4OSA_TRUE;
        }
        else
        {
            M4ERR_EXIT(M4NO_ERROR);
        }
    }

    return M4NO_ERROR;


/* ----- Release resources if an error occured */
exit_with_error:

    /* Abort decoding */
    pStreamContext->m_bDataDecodePending = M4OSA_FALSE;

    if((M4NO_ERROR == pStreamContext->m_uiDecodeError) && (M4NO_ERROR != err))
    {
        pStreamContext->m_uiDecodeError = err;
    }

    return err;
}

/**
 ************************************************************************
 * @brief
 * @note
 *
 * @param    pVS_Context:    (IN)    Context of the video hw shell
 *
 * @return    M4NO_ERROR        There is no error
 ************************************************************************
 */
static M4OSA_ERR M4DECODER_EXTERNAL_AsynchronousRender(M4OSA_Context pVS_Context)
{
    M4VS_VideoDecoder_Context* pStreamContext = (M4VS_VideoDecoder_Context*)pVS_Context;

    M4OSA_ERR err = M4NO_ERROR;


    /* ----- Render one frame ----- */

    if(M4OSA_TRUE == pStreamContext->m_bDataRenderPending)
    {
#if 0
        if (!pStreamContext->m_bForceRender)
        {
            /* Targeted time is reached */
            if(pStreamContext->m_TargetRenderCts - pStreamContext->m_CurrentRenderCts < 1.0)
             /* some +0.5 issues */
            {
                M4OSA_TRACE2_0("M4DECODER_EXTERNAL_AsynchronousRender :\
                     skip render because synchronisation");
                pStreamContext->m_uiRenderError = M4WAR_RENDER_FINISHED;

                M4ERR_EXIT(M4NO_ERROR);
            }

            if ( (M4WAR_NO_MORE_AU == pStreamContext->m_uiDecodeError)
                && (pStreamContext->m_CurrentDecodeCts \
                    - pStreamContext->m_CurrentRenderCts < 1.0) )
            {
                pStreamContext->m_uiRenderError = M4WAR_RENDER_FINISHED;
                M4ERR_EXIT(M4NO_ERROR);
            }

            if(pStreamContext->m_NbDecodedFrames == 0)
            {
                pStreamContext->m_uiRenderError = M4WAR_VIDEORENDERER_NO_NEW_FRAME;
                M4ERR_EXIT(M4NO_ERROR);
            }
        }
#endif
        /* Render the frame */
        pStreamContext->m_CurrentRenderCts = pStreamContext->m_CurrentDecodeCts;

        pStreamContext->m_uiRenderError =
             pStreamContext->m_VD_Interface->m_pFctStepRender(pStreamContext->m_VD_Context,
                 pStreamContext->m_pOutputPlane, pStreamContext->m_CurrentRenderCts);
#ifndef M4DECODER_EXTERNAL_SYNC_EXT_DECODE
        if ( (M4NO_ERROR == pStreamContext->m_uiRenderError)
            /* || (M4WAR_IO_PENDING == pStreamContext->m_uiRenderError) */ )
        {
            /* wait for render to complete */
            //M4semvalue--;
            //printf("Semaphore wait: %d\n", M4semvalue);
            M4OSA_semaphoreWait(pStreamContext->m_SemSync, M4OSA_WAIT_FOREVER);
            /* by now the actual m_uiRenderError has been set by signalRender */
        }
#endif /* not M4DECODER_EXTERNAL_SYNC_EXT_DECODE */
        if(M4NO_ERROR != pStreamContext->m_uiRenderError)
        {
            M4OSA_TRACE1_1("M4DECODER_EXTERNAL_AsynchronousRender : HW render error 0x%x", err);
            pStreamContext->m_bDataRenderPending = M4OSA_FALSE;

            return M4NO_ERROR;
        }

        /* Launch in asynchronous mode the predecoding of the next frame */
        pStreamContext->m_NbDecodedFrames = 0;
        pStreamContext->m_uiDecodeError = M4NO_ERROR;
        pStreamContext->m_bDataDecodePending = M4OSA_TRUE;
        M4DECODER_EXTERNAL_AsynchronousDecode(pVS_Context);

        pStreamContext->m_uiRenderError = M4WAR_RENDER_FINISHED;
    }

    return M4NO_ERROR;


/* ----- Release resources if an error occured */
exit_with_error:

    /* Abort the rendering */
    pStreamContext->m_bDataRenderPending = M4OSA_FALSE;

    if((M4NO_ERROR == pStreamContext->m_uiRenderError) && (M4NO_ERROR != err))
    {
        pStreamContext->m_uiRenderError = err;
    }


    return err;
}

