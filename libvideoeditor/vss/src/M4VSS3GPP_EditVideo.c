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
 * @file    M4VSS3GPP_EditVideo.c
 * @brief    Video Studio Service 3GPP edit API implementation.
 * @note
 ******************************************************************************
 */

/****************/
/*** Includes ***/
/****************/

#include "NXPSW_CompilerSwitches.h"
/**
 * Our header */
#include "M4VSS3GPP_API.h"
#include "M4VSS3GPP_InternalTypes.h"
#include "M4VSS3GPP_InternalFunctions.h"
#include "M4VSS3GPP_InternalConfig.h"
#include "M4VSS3GPP_ErrorCodes.h"

// StageFright encoders require %16 resolution
#include "M4ENCODER_common.h"
/**
 * OSAL headers */
#include "M4OSA_Memory.h" /**< OSAL memory management */
#include "M4OSA_Debug.h"  /**< OSAL debug management */

/**
 * component includes */
#include "M4VFL_transition.h" /**< video effects */

/*for transition behaviour*/
#include <math.h>

/************************************************************************/
/* Static local functions                                               */
/************************************************************************/

static M4OSA_ERR M4VSS3GPP_intCheckVideoMode(
    M4VSS3GPP_InternalEditContext *pC );
static M4OSA_Void
M4VSS3GPP_intCheckVideoEffects( M4VSS3GPP_InternalEditContext *pC,
                               M4OSA_UInt8 uiClipNumber );
static M4OSA_ERR
M4VSS3GPP_intApplyVideoEffect( M4VSS3GPP_InternalEditContext *pC,/*M4OSA_UInt8 uiClip1orClip2,*/
                              M4VIFI_ImagePlane *pPlaneIn, M4VIFI_ImagePlane *pPlaneOut );
static M4OSA_ERR
M4VSS3GPP_intVideoTransition( M4VSS3GPP_InternalEditContext *pC,
                             M4VIFI_ImagePlane *pPlaneOut );

static M4OSA_Void
M4VSS3GPP_intUpdateTimeInfo( M4VSS3GPP_InternalEditContext *pC,
                            M4SYS_AccessUnit *pAU );
static M4OSA_Void M4VSS3GPP_intSetH263TimeCounter( M4OSA_MemAddr8 pAuDataBuffer,
                                                  M4OSA_UInt8 uiCts );
static M4OSA_Void M4VSS3GPP_intSetMPEG4Gov( M4OSA_MemAddr8 pAuDataBuffer,
                                           M4OSA_UInt32 uiCtsSec );
static M4OSA_Void M4VSS3GPP_intGetMPEG4Gov( M4OSA_MemAddr8 pAuDataBuffer,
                                           M4OSA_UInt32 *pCtsSec );
static M4OSA_ERR M4VSS3GPP_intAllocateYUV420( M4VIFI_ImagePlane *pPlanes,
                                             M4OSA_UInt32 uiWidth, M4OSA_UInt32 uiHeight );

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intEditStepVideo()
 * @brief    One step of video processing
 * @param   pC    (IN/OUT) Internal edit context
 ******************************************************************************
 */
M4OSA_ERR M4VSS3GPP_intEditStepVideo( M4VSS3GPP_InternalEditContext *pC )
{
    M4OSA_ERR err;
    M4OSA_Int32 iCts, iNextCts;
    M4ENCODER_FrameMode FrameMode;
    M4OSA_Bool bSkipFrame;
    M4OSA_UInt16 offset;

    /**
     * Check if we reached end cut. Decorrelate input and output encoding
     * timestamp to handle encoder prefetch
     */
    if ( ((M4OSA_Int32)(pC->ewc.dInputVidCts) - pC->pC1->iVoffset
        + pC->iInOutTimeOffset) >= pC->pC1->iEndTime )
    {
        /* Re-adjust video to precise cut time */
        pC->iInOutTimeOffset = ((M4OSA_Int32)(pC->ewc.dInputVidCts))
            - pC->pC1->iVoffset + pC->iInOutTimeOffset - pC->pC1->iEndTime;
        if ( pC->iInOutTimeOffset < 0 ) {
            pC->iInOutTimeOffset = 0;
        }

        /**
        * Video is done for this clip */
        err = M4VSS3GPP_intReachedEndOfVideo(pC);

        /* RC: to know when a file has been processed */
        if (M4NO_ERROR != err && err != M4VSS3GPP_WAR_SWITCH_CLIP)
        {
            M4OSA_TRACE1_1(
                "M4VSS3GPP_intEditStepVideo: M4VSS3GPP_intReachedEndOfVideo returns 0x%x",
                err);
        }

        return err;
    }

    /* Don't change the states if we are in decodeUpTo() */
    if ( (M4VSS3GPP_kClipStatus_DECODE_UP_TO != pC->pC1->Vstatus)
        && (( pC->pC2 == M4OSA_NULL)
        || (M4VSS3GPP_kClipStatus_DECODE_UP_TO != pC->pC2->Vstatus)) )
    {
        /**
        * Check Video Mode, depending on the current output CTS */
        err = M4VSS3GPP_intCheckVideoMode(
            pC); /**< This function change the pC->Vstate variable! */

        if (M4NO_ERROR != err)
        {
            M4OSA_TRACE1_1(
                "M4VSS3GPP_intEditStepVideo: M4VSS3GPP_intCheckVideoMode returns 0x%x!",
                err);
            return err;
        }
    }


    switch( pC->Vstate )
    {
        /* _________________ */
        /*|                 |*/
        /*| READ_WRITE MODE |*/
        /*|_________________|*/

        case M4VSS3GPP_kEditVideoState_READ_WRITE:
        case M4VSS3GPP_kEditVideoState_AFTER_CUT:
            {
                M4OSA_TRACE3_0("M4VSS3GPP_intEditStepVideo READ_WRITE");

                bSkipFrame = M4OSA_FALSE;

                /**
                * If we were decoding the clip, we must jump to be sure
                * to get to the good position. */
                if( M4VSS3GPP_kClipStatus_READ != pC->pC1->Vstatus )
                {
                    /**
                    * Jump to target video time (tc = to-T) */
                // Decorrelate input and output encoding timestamp to handle encoder prefetch
                iCts = (M4OSA_Int32)(pC->ewc.dInputVidCts) - pC->pC1->iVoffset;
                    err = pC->pC1->ShellAPI.m_pReader->m_pFctJump(
                        pC->pC1->pReaderContext,
                        (M4_StreamHandler *)pC->pC1->pVideoStream, &iCts);

                    if( M4NO_ERROR != err )
                    {
                        M4OSA_TRACE1_1(
                            "M4VSS3GPP_intEditStepVideo:\
                            READ_WRITE: m_pReader->m_pFctJump(V1) returns 0x%x!",
                            err);
                        return err;
                    }

                    err = pC->pC1->ShellAPI.m_pReaderDataIt->m_pFctGetNextAu(
                        pC->pC1->pReaderContext,
                        (M4_StreamHandler *)pC->pC1->pVideoStream,
                        &pC->pC1->VideoAU);

                    if( ( M4NO_ERROR != err) && (M4WAR_NO_MORE_AU != err) )
                    {
                        M4OSA_TRACE1_1(
                            "M4VSS3GPP_intEditStepVideo:\
                            READ_WRITE: m_pReader->m_pFctGetNextAu returns 0x%x!",
                            err);
                        return err;
                    }

                    M4OSA_TRACE2_3("A .... read  : cts  = %.0f + %ld [ 0x%x ]",
                        pC->pC1->VideoAU.m_CTS, pC->pC1->iVoffset,
                        pC->pC1->VideoAU.m_size);

                    /* This frame has been already written in BEGIN CUT step -> skip it */
                    if( pC->pC1->VideoAU.m_CTS == iCts
                        && pC->pC1->iVideoRenderCts >= iCts )
                    {
                        bSkipFrame = M4OSA_TRUE;
                    }
                }

                /* This frame has been already written in BEGIN CUT step -> skip it */
                if( ( pC->Vstate == M4VSS3GPP_kEditVideoState_AFTER_CUT)
                    && (pC->pC1->VideoAU.m_CTS
                    + pC->pC1->iVoffset <= pC->ewc.WriterVideoAU.CTS) )
                {
                    bSkipFrame = M4OSA_TRUE;
                }

                /**
                * Remember the clip reading state */
                pC->pC1->Vstatus = M4VSS3GPP_kClipStatus_READ;
                // Decorrelate input and output encoding timestamp to handle encoder prefetch
                // Rounding is to compensate reader imprecision (m_CTS is actually an integer)
                iCts = ((M4OSA_Int32)pC->ewc.dInputVidCts) - pC->pC1->iVoffset - 1;
                iNextCts = iCts + ((M4OSA_Int32)pC->dOutputFrameDuration) + 1;
                /* Avoid to write a last frame of duration 0 */
                if( iNextCts > pC->pC1->iEndTime )
                    iNextCts = pC->pC1->iEndTime;

                /**
                * If the AU is good to be written, write it, else just skip it */
                if( ( M4OSA_FALSE == bSkipFrame)
                    && (( pC->pC1->VideoAU.m_CTS >= iCts)
                    && (pC->pC1->VideoAU.m_CTS < iNextCts)
                    && (pC->pC1->VideoAU.m_size > 0)) )
                {
                    /**
                    * Get the output AU to write into */
                    err = pC->ShellAPI.pWriterDataFcts->pStartAU(
                        pC->ewc.p3gpWriterContext,
                        M4VSS3GPP_WRITER_VIDEO_STREAM_ID,
                        &pC->ewc.WriterVideoAU);

                    if( M4NO_ERROR != err )
                    {
                        M4OSA_TRACE1_1(
                            "M4VSS3GPP_intEditStepVideo: READ_WRITE:\
                            pWriterDataFcts->pStartAU(Video) returns 0x%x!",
                            err);
                        return err;
                    }

                    /**
                    * Copy the input AU to the output AU */
                    pC->ewc.WriterVideoAU.attribute = pC->pC1->VideoAU.m_attribute;
                    // Decorrelate input and output encoding timestamp to handle encoder prefetch
                    pC->ewc.WriterVideoAU.CTS = (M4OSA_Time)pC->pC1->VideoAU.m_CTS +
                        (M4OSA_Time)pC->pC1->iVoffset;
                    pC->ewc.dInputVidCts += pC->dOutputFrameDuration;
                    offset = 0;
                    /* for h.264 stream do not read the 1st 4 bytes as they are header
                     indicators */
                    if( pC->pC1->pVideoStream->m_basicProperties.m_streamType
                        == M4DA_StreamTypeVideoMpeg4Avc )
                        offset = 4;

                    pC->ewc.WriterVideoAU.size = pC->pC1->VideoAU.m_size - offset;
                    if( pC->ewc.WriterVideoAU.size > pC->ewc.uiVideoMaxAuSize )
                    {
                        M4OSA_TRACE1_2(
                            "M4VSS3GPP_intEditStepVideo: READ_WRITE: AU size greater than\
                             MaxAuSize (%d>%d)! returning M4VSS3GPP_ERR_INPUT_VIDEO_AU_TOO_LARGE",
                            pC->ewc.WriterVideoAU.size, pC->ewc.uiVideoMaxAuSize);
                        return M4VSS3GPP_ERR_INPUT_VIDEO_AU_TOO_LARGE;
                    }

                    memcpy((void *)pC->ewc.WriterVideoAU.dataAddress,
                        (void *)(pC->pC1->VideoAU.m_dataAddress + offset),
                        (pC->ewc.WriterVideoAU.size));

                    /**
                    * Update time info for the Counter Time System to be equal to the bit
                    -stream time*/
                    M4VSS3GPP_intUpdateTimeInfo(pC, &pC->ewc.WriterVideoAU);
                    M4OSA_TRACE2_2("B ---- write : cts  = %lu [ 0x%x ]",
                        pC->ewc.WriterVideoAU.CTS, pC->ewc.WriterVideoAU.size);

                    /**
                    * Write the AU */
                    err = pC->ShellAPI.pWriterDataFcts->pProcessAU(
                        pC->ewc.p3gpWriterContext,
                        M4VSS3GPP_WRITER_VIDEO_STREAM_ID,
                        &pC->ewc.WriterVideoAU);

                    if( M4NO_ERROR != err )
                    {
                        /* the warning M4WAR_WRITER_STOP_REQ is returned when the targeted output
                         file size is reached
                        The editing is then finished, the warning M4VSS3GPP_WAR_EDITING_DONE
                        is returned*/
                        if( M4WAR_WRITER_STOP_REQ == err )
                        {
                            M4OSA_TRACE1_0(
                                "M4VSS3GPP_intEditStepVideo: File was cut to avoid oversize");
                            return M4VSS3GPP_WAR_EDITING_DONE;
                        }
                        else
                        {
                            M4OSA_TRACE1_1(
                                "M4VSS3GPP_intEditStepVideo: READ_WRITE:\
                                pWriterDataFcts->pProcessAU(Video) returns 0x%x!",
                                err);
                            return err;
                        }
                    }

                    /**
                    * Read next AU for next step */
                    err = pC->pC1->ShellAPI.m_pReaderDataIt->m_pFctGetNextAu(
                        pC->pC1->pReaderContext,
                        (M4_StreamHandler *)pC->pC1->pVideoStream,
                        &pC->pC1->VideoAU);

                    if( ( M4NO_ERROR != err) && (M4WAR_NO_MORE_AU != err) )
                    {
                        M4OSA_TRACE1_1(
                            "M4VSS3GPP_intEditStepVideo: READ_WRITE:\
                            m_pReaderDataIt->m_pFctGetNextAu returns 0x%x!",
                            err);
                        return err;
                    }

                    M4OSA_TRACE2_3("C .... read  : cts  = %.0f + %ld [ 0x%x ]",
                        pC->pC1->VideoAU.m_CTS, pC->pC1->iVoffset,
                        pC->pC1->VideoAU.m_size);
                }
                else
                {
                    /**
                    * Decide wether to read or to increment time increment */
                    if( ( pC->pC1->VideoAU.m_size == 0)
                        || (pC->pC1->VideoAU.m_CTS >= iNextCts) )
                    {
                        /*Increment time by the encoding period (NO_MORE_AU or reader in advance */
                       // Decorrelate input and output encoding timestamp to handle encoder prefetch
                       pC->ewc.dInputVidCts += pC->dOutputFrameDuration;

                        /* Switch (from AFTER_CUT) to normal mode because time is
                        no more frozen */
                        pC->Vstate = M4VSS3GPP_kEditVideoState_READ_WRITE;
                    }
                    else
                    {
                        /* In other cases (reader late), just let the reader catch up
                         pC->ewc.dVTo */
                        err = pC->pC1->ShellAPI.m_pReaderDataIt->m_pFctGetNextAu(
                            pC->pC1->pReaderContext,
                            (M4_StreamHandler *)pC->pC1->pVideoStream,
                            &pC->pC1->VideoAU);

                        if( ( M4NO_ERROR != err) && (M4WAR_NO_MORE_AU != err) )
                        {
                            M4OSA_TRACE1_1(
                                "M4VSS3GPP_intEditStepVideo: READ_WRITE:\
                                m_pReaderDataIt->m_pFctGetNextAu returns 0x%x!",
                                err);
                            return err;
                        }

                        M4OSA_TRACE2_3("D .... read  : cts  = %.0f + %ld [ 0x%x ]",
                            pC->pC1->VideoAU.m_CTS, pC->pC1->iVoffset,
                            pC->pC1->VideoAU.m_size);
                    }
                }
            }
            break;

            /* ____________________ */
            /*|                    |*/
            /*| DECODE_ENCODE MODE |*/
            /*|   BEGIN_CUT MODE   |*/
            /*|____________________|*/

        case M4VSS3GPP_kEditVideoState_DECODE_ENCODE:
        case M4VSS3GPP_kEditVideoState_BEGIN_CUT:
            {
                M4OSA_TRACE3_0(
                    "M4VSS3GPP_intEditStepVideo DECODE_ENCODE / BEGIN_CUT");

                /**
                * Decode the video up to the target time
                (will jump to the previous RAP if needed ) */
                // Decorrelate input and output encoding timestamp to handle encoder prefetch
                err = M4VSS3GPP_intClipDecodeVideoUpToCts(pC->pC1, (M4OSA_Int32)pC->ewc.dInputVidCts);
                if( M4NO_ERROR != err )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intEditStepVideo: DECODE_ENCODE:\
                        M4VSS3GPP_intDecodeVideoUpToCts returns err=0x%x",
                        err);
                    return err;
                }

                /* If the decoding is not completed, do one more step with time frozen */
                if( M4VSS3GPP_kClipStatus_DECODE_UP_TO == pC->pC1->Vstatus )
                {
                    return M4NO_ERROR;
                }

                /**
                * Reset the video pre-processing error before calling the encoder */
                pC->ewc.VppError = M4NO_ERROR;

                M4OSA_TRACE2_0("E ++++ encode AU");

                /**
                * Encode the frame(rendering,filtering and writing will be done
                 in encoder callbacks)*/
                if( pC->Vstate == M4VSS3GPP_kEditVideoState_BEGIN_CUT )
                    FrameMode = M4ENCODER_kIFrame;
                else
                    FrameMode = M4ENCODER_kNormalFrame;

                // Decorrelate input and output encoding timestamp to handle encoder prefetch
                err = pC->ShellAPI.pVideoEncoderGlobalFcts->pFctEncode(pC->ewc.pEncContext, M4OSA_NULL,
                pC->ewc.dInputVidCts, FrameMode);
                /**
                * Check if we had a VPP error... */
                if( M4NO_ERROR != pC->ewc.VppError )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intEditStepVideo: DECODE_ENCODE:\
                        pVideoEncoderGlobalFcts->pFctEncode, returning VppErr=0x%x",
                        pC->ewc.VppError);
#ifdef M4VSS_SUPPORT_OMX_CODECS

                    if( M4WAR_VIDEORENDERER_NO_NEW_FRAME != pC->ewc.VppError )
                    {
#endif //M4VSS_SUPPORT_OMX_CODECS

                        return pC->ewc.VppError;
#ifdef M4VSS_SUPPORT_OMX_CODECS

                    }

#endif                                   //M4VSS_SUPPORT_OMX_CODECS

                }
                else if( M4NO_ERROR != err ) /**< ...or an encoder error */
                {
                    if( ((M4OSA_UInt32)M4ERR_ALLOC) == err )
                    {
                        M4OSA_TRACE1_0(
                            "M4VSS3GPP_intEditStepVideo: DECODE_ENCODE:\
                            returning M4VSS3GPP_ERR_ENCODER_ACCES_UNIT_ERROR");
                        return M4VSS3GPP_ERR_ENCODER_ACCES_UNIT_ERROR;
                    }
                    /* the warning M4WAR_WRITER_STOP_REQ is returned when the targeted output
                    file size is reached
                    The editing is then finished, the warning M4VSS3GPP_WAR_EDITING_DONE
                    is returned*/
                    else if( M4WAR_WRITER_STOP_REQ == err )
                    {
                        M4OSA_TRACE1_0(
                            "M4VSS3GPP_intEditStepVideo: File was cut to avoid oversize");
                        return M4VSS3GPP_WAR_EDITING_DONE;
                    }
                    else
                    {
                        M4OSA_TRACE1_1(
                            "M4VSS3GPP_intEditStepVideo: DECODE_ENCODE:\
                            pVideoEncoderGlobalFcts->pFctEncode returns 0x%x",
                            err);
                        return err;
                    }
                }

                /**
                * Increment time by the encoding period (for begin cut, do not increment to not
                loose P-frames) */
                if( M4VSS3GPP_kEditVideoState_DECODE_ENCODE == pC->Vstate )
                {
                    // Decorrelate input and output encoding timestamp to handle encoder prefetch
                    pC->ewc.dInputVidCts += pC->dOutputFrameDuration;
                }
            }
            break;

            /* _________________ */
            /*|                 |*/
            /*| TRANSITION MODE |*/
            /*|_________________|*/

        case M4VSS3GPP_kEditVideoState_TRANSITION:
            {
                M4OSA_TRACE3_0("M4VSS3GPP_intEditStepVideo TRANSITION");

                /* Don't decode more than needed */
                if( !(( M4VSS3GPP_kClipStatus_DECODE_UP_TO != pC->pC1->Vstatus)
                    && (M4VSS3GPP_kClipStatus_DECODE_UP_TO == pC->pC2->Vstatus)) )
                {
                    /**
                    * Decode the clip1 video up to the target time
                    (will jump to the previous RAP if needed */
                    // Decorrelate input and output encoding timestamp to handle encoder prefetch
                    err = M4VSS3GPP_intClipDecodeVideoUpToCts(pC->pC1,
                         (M4OSA_Int32)pC->ewc.dInputVidCts);
                    if( M4NO_ERROR != err )
                    {
                        M4OSA_TRACE1_1(
                            "M4VSS3GPP_intEditStepVideo: TRANSITION:\
                            M4VSS3GPP_intDecodeVideoUpToCts(C1) returns err=0x%x",
                            err);
                        return err;
                    }

                    /* If the decoding is not completed, do one more step with time frozen */
                    if( M4VSS3GPP_kClipStatus_DECODE_UP_TO == pC->pC1->Vstatus )
                    {
                        return M4NO_ERROR;
                    }
                }

                /* Don't decode more than needed */
                if( !(( M4VSS3GPP_kClipStatus_DECODE_UP_TO != pC->pC2->Vstatus)
                    && (M4VSS3GPP_kClipStatus_DECODE_UP_TO == pC->pC1->Vstatus)) )
                {
                    /**
                    * Decode the clip2 video up to the target time
                        (will jump to the previous RAP if needed) */
                    // Decorrelate input and output encoding timestamp to handle encoder prefetch
                    err = M4VSS3GPP_intClipDecodeVideoUpToCts(pC->pC2,
                         (M4OSA_Int32)pC->ewc.dInputVidCts);
                    if( M4NO_ERROR != err )
                    {
                        M4OSA_TRACE1_1(
                            "M4VSS3GPP_intEditStepVideo: TRANSITION:\
                            M4VSS3GPP_intDecodeVideoUpToCts(C2) returns err=0x%x",
                            err);
                        return err;
                    }

                    /* If the decoding is not completed, do one more step with time frozen */
                    if( M4VSS3GPP_kClipStatus_DECODE_UP_TO == pC->pC2->Vstatus )
                    {
                        return M4NO_ERROR;
                    }
                }

                /**
                * Reset the video pre-processing error before calling the encoder */
                pC->ewc.VppError = M4NO_ERROR;

                M4OSA_TRACE2_0("F **** blend AUs");

                /**
                * Encode the frame (rendering, filtering and writing will be done
                in encoder callbacks */
                // Decorrelate input and output encoding timestamp to handle encoder prefetch
                err = pC->ShellAPI.pVideoEncoderGlobalFcts->pFctEncode(pC->ewc.pEncContext, M4OSA_NULL,
                    pC->ewc.dInputVidCts, M4ENCODER_kNormalFrame);

                /**
                * If encode returns a process frame error, it is likely to be a VPP error */
                if( M4NO_ERROR != pC->ewc.VppError )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intEditStepVideo: TRANSITION:\
                        pVideoEncoderGlobalFcts->pFctEncode, returning VppErr=0x%x",
                        pC->ewc.VppError);
#ifdef M4VSS_SUPPORT_OMX_CODECS

                    if( M4WAR_VIDEORENDERER_NO_NEW_FRAME != pC->ewc.VppError )
                    {

#endif //M4VSS_SUPPORT_OMX_CODECS

                        return pC->ewc.VppError;
#ifdef M4VSS_SUPPORT_OMX_CODECS

                    }

#endif //M4VSS_SUPPORT_OMX_CODECS

                }
                else if( M4NO_ERROR != err ) /**< ...or an encoder error */
                {
                    if( ((M4OSA_UInt32)M4ERR_ALLOC) == err )
                    {
                        M4OSA_TRACE1_0(
                            "M4VSS3GPP_intEditStepVideo: TRANSITION:\
                            returning M4VSS3GPP_ERR_ENCODER_ACCES_UNIT_ERROR");
                        return M4VSS3GPP_ERR_ENCODER_ACCES_UNIT_ERROR;
                    }

                    /* the warning M4WAR_WRITER_STOP_REQ is returned when the targeted output
                     file size is reached
                    The editing is then finished, the warning M4VSS3GPP_WAR_EDITING_DONE is
                     returned*/
                    else if( M4WAR_WRITER_STOP_REQ == err )
                    {
                        M4OSA_TRACE1_0(
                            "M4VSS3GPP_intEditStepVideo: File was cut to avoid oversize");
                        return M4VSS3GPP_WAR_EDITING_DONE;
                    }
                    else
                    {
                        M4OSA_TRACE1_1(
                            "M4VSS3GPP_intEditStepVideo: TRANSITION:\
                            pVideoEncoderGlobalFcts->pFctEncode returns 0x%x",
                            err);
                        return err;
                    }
                }

                /**
                * Increment time by the encoding period */
                // Decorrelate input and output encoding timestamp to handle encoder prefetch
                pC->ewc.dInputVidCts += pC->dOutputFrameDuration;
            }
            break;

            /* ____________ */
            /*|            |*/
            /*| ERROR CASE |*/
            /*|____________|*/

        default:
            M4OSA_TRACE1_1(
                "M4VSS3GPP_intEditStepVideo: invalid internal state (0x%x),\
                returning M4VSS3GPP_ERR_INTERNAL_STATE",
                pC->Vstate);
            return M4VSS3GPP_ERR_INTERNAL_STATE;
    }

    /**
    * Return with no error */
    M4OSA_TRACE3_0("M4VSS3GPP_intEditStepVideo: returning M4NO_ERROR");
    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intCheckVideoMode()
 * @brief    Check which video process mode we must use, depending on the output CTS.
 * @param   pC    (IN/OUT) Internal edit context
 ******************************************************************************
 */
static M4OSA_ERR M4VSS3GPP_intCheckVideoMode(
    M4VSS3GPP_InternalEditContext *pC )
{
    M4OSA_ERR err;
    // Decorrelate input and output encoding timestamp to handle encoder prefetch
    const M4OSA_Int32  t = (M4OSA_Int32)pC->ewc.dInputVidCts;
    /**< Transition duration */
    const M4OSA_Int32 TD = pC->pTransitionList[pC->uiCurrentClip].uiTransitionDuration;

    M4OSA_Int32 iTmp;

    const M4VSS3GPP_EditVideoState previousVstate = pC->Vstate;

    /**
    * Check if Clip1 is on its begin cut, or in an effect zone */
    M4VSS3GPP_intCheckVideoEffects(pC, 1);

    /**
    * Check if we are in the transition with next clip */
    if( ( TD > 0) && (( t - pC->pC1->iVoffset) >= (pC->pC1->iEndTime - TD)) )
    {
        /**
        * We are in a transition */
        pC->Vstate = M4VSS3GPP_kEditVideoState_TRANSITION;
        pC->bTransitionEffect = M4OSA_TRUE;

        /**
        * Open second clip for transition, if not yet opened */
        if( M4OSA_NULL == pC->pC2 )
        {
            err = M4VSS3GPP_intOpenClip(pC, &pC->pC2,
                &pC->pClipList[pC->uiCurrentClip + 1]);

            if( M4NO_ERROR != err )
            {
                M4OSA_TRACE1_1(
                    "M4VSS3GPP_intCheckVideoMode: M4VSS3GPP_editOpenClip returns 0x%x!",
                    err);
                return err;
            }

            /**
            * Add current video output CTS to the clip offset
            * (audio output CTS is not yet at the transition, so audio
            *  offset can't be updated yet). */
            // Decorrelate input and output encoding timestamp to handle encoder prefetch
            pC->pC2->iVoffset += (M4OSA_UInt32)pC->ewc.dInputVidCts;

            /**
            * 2005-03-24: BugFix for audio-video synchro:
            * Update transition duration due to the actual video transition beginning time.
            * It will avoid desynchronization when doing the audio transition. */
           // Decorrelate input and output encoding timestamp to handle encoder prefetch
            iTmp = ((M4OSA_Int32)pC->ewc.dInputVidCts)\
             - (pC->pC1->iEndTime - TD + pC->pC1->iVoffset);
            if (iTmp < (M4OSA_Int32)pC->pTransitionList[pC->uiCurrentClip].uiTransitionDuration)
            /**< Test in case of a very short transition */
            {
                pC->pTransitionList[pC->
                    uiCurrentClip].uiTransitionDuration -= iTmp;

                /**
                * Don't forget to also correct the total duration used for the progress bar
                * (it was computed with the original transition duration). */
                pC->ewc.iOutputDuration += iTmp;
            }
            /**< No "else" here because it's hard predict the effect of 0 duration transition...*/
        }

        /**
        * Check effects for clip2 */
        M4VSS3GPP_intCheckVideoEffects(pC, 2);
    }
    else
    {
        /**
        * We are not in a transition */
        pC->bTransitionEffect = M4OSA_FALSE;

        /* If there is an effect we go to decode/encode mode */
        if ((pC->nbActiveEffects > 0) ||(pC->nbActiveEffects1 > 0))
        {
            pC->Vstate = M4VSS3GPP_kEditVideoState_DECODE_ENCODE;
        }
        /* We do a begin cut, except if already done (time is not progressing because we want
        to catch all P-frames after the cut) */
        else if( M4OSA_TRUE == pC->bClip1AtBeginCut )
        {
            if(pC->pC1->pSettings->ClipProperties.VideoStreamType == M4VIDEOEDITING_kH264) {
                pC->Vstate = M4VSS3GPP_kEditVideoState_DECODE_ENCODE;
                pC->bEncodeTillEoF = M4OSA_TRUE;
            } else if( ( M4VSS3GPP_kEditVideoState_BEGIN_CUT == previousVstate)
                || (M4VSS3GPP_kEditVideoState_AFTER_CUT == previousVstate) ) {
                pC->Vstate = M4VSS3GPP_kEditVideoState_AFTER_CUT;
            } else {
                pC->Vstate = M4VSS3GPP_kEditVideoState_BEGIN_CUT;
            }
        }
        /* Else we are in default copy/paste mode */
        else
        {
            if( ( M4VSS3GPP_kEditVideoState_BEGIN_CUT == previousVstate)
                || (M4VSS3GPP_kEditVideoState_AFTER_CUT == previousVstate) )
            {
                pC->Vstate = M4VSS3GPP_kEditVideoState_AFTER_CUT;
            }
            else if( pC->bIsMMS == M4OSA_TRUE )
            {
                M4OSA_UInt32 currentBitrate;
                M4OSA_ERR err = M4NO_ERROR;

                /* Do we need to reencode the video to downgrade the bitrate or not ? */
                /* Let's compute the cirrent bitrate of the current edited clip */
                err = pC->pC1->ShellAPI.m_pReader->m_pFctGetOption(
                    pC->pC1->pReaderContext,
                    M4READER_kOptionID_Bitrate, &currentBitrate);

                if( err != M4NO_ERROR )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intCheckVideoMode:\
                        Error when getting next bitrate of edited clip: 0x%x",
                        err);
                    return err;
                }

                /* Remove audio bitrate */
                currentBitrate -= 12200;

                /* Test if we go into copy/paste mode or into decode/encode mode */
                if( currentBitrate > pC->uiMMSVideoBitrate )
                {
                    pC->Vstate = M4VSS3GPP_kEditVideoState_DECODE_ENCODE;
                }
                else
                {
                    pC->Vstate = M4VSS3GPP_kEditVideoState_READ_WRITE;
                }
            }
            else if(!((pC->m_bClipExternalHasStarted == M4OSA_TRUE) &&
                    (pC->Vstate == M4VSS3GPP_kEditVideoState_DECODE_ENCODE)) &&
                    pC->bEncodeTillEoF == M4OSA_FALSE)
            {
                /**
                 * Test if we go into copy/paste mode or into decode/encode mode
                 * If an external effect has been applied on the current clip
                 * then continue to be in decode/encode mode till end of
                 * clip to avoid H.264 distortion.
                 */
                pC->Vstate = M4VSS3GPP_kEditVideoState_READ_WRITE;
            }
        }
    }

    /**
    * Check if we create an encoder */
    if( ( ( M4VSS3GPP_kEditVideoState_READ_WRITE == previousVstate)
        || (M4VSS3GPP_kEditVideoState_AFTER_CUT
        == previousVstate)) /**< read mode */
        && (( M4VSS3GPP_kEditVideoState_DECODE_ENCODE == pC->Vstate)
        || (M4VSS3GPP_kEditVideoState_BEGIN_CUT == pC->Vstate)
        || (M4VSS3GPP_kEditVideoState_TRANSITION
        == pC->Vstate)) /**< encode mode */
        && pC->bIsMMS == M4OSA_FALSE )
    {
        /**
        * Create the encoder */
        err = M4VSS3GPP_intCreateVideoEncoder(pC);

        if( M4NO_ERROR != err )
        {
            M4OSA_TRACE1_1(
                "M4VSS3GPP_intCheckVideoMode: M4VSS3GPP_intCreateVideoEncoder returns 0x%x!",
                err);
            return err;
        }
    }
    else if( pC->bIsMMS == M4OSA_TRUE && pC->ewc.pEncContext == M4OSA_NULL )
    {
        /**
        * Create the encoder */
        err = M4VSS3GPP_intCreateVideoEncoder(pC);

        if( M4NO_ERROR != err )
        {
            M4OSA_TRACE1_1(
                "M4VSS3GPP_intCheckVideoMode: M4VSS3GPP_intCreateVideoEncoder returns 0x%x!",
                err);
            return err;
        }
    }

    /**
    * When we go from filtering to read/write, we must act like a begin cut,
    * because the last filtered image may be different than the original image. */
    else if( ( ( M4VSS3GPP_kEditVideoState_DECODE_ENCODE == previousVstate)
        || (M4VSS3GPP_kEditVideoState_TRANSITION
        == previousVstate)) /**< encode mode */
        && (M4VSS3GPP_kEditVideoState_READ_WRITE == pC->Vstate) /**< read mode */
        && (pC->bEncodeTillEoF == M4OSA_FALSE) )
    {
        pC->Vstate = M4VSS3GPP_kEditVideoState_BEGIN_CUT;
    }

    /**
    * Check if we destroy an encoder */
    else if( ( ( M4VSS3GPP_kEditVideoState_DECODE_ENCODE == previousVstate)
        || (M4VSS3GPP_kEditVideoState_BEGIN_CUT == previousVstate)
        || (M4VSS3GPP_kEditVideoState_TRANSITION
        == previousVstate)) /**< encode mode */
        && (( M4VSS3GPP_kEditVideoState_READ_WRITE == pC->Vstate)
        || (M4VSS3GPP_kEditVideoState_AFTER_CUT
        == pC->Vstate)) /**< read mode */
        && pC->bIsMMS == M4OSA_FALSE )
    {
        /**
        * Destroy the previously created encoder */
        err = M4VSS3GPP_intDestroyVideoEncoder(pC);

        if( M4NO_ERROR != err )
        {
            M4OSA_TRACE1_1(
                "M4VSS3GPP_intCheckVideoMode: M4VSS3GPP_intDestroyVideoEncoder returns 0x%x!",
                err);
            return err;
        }
    }

    /**
    * Return with no error */
    M4OSA_TRACE3_0("M4VSS3GPP_intCheckVideoMode: returning M4NO_ERROR");
    return M4NO_ERROR;
}

/******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intStartAU()
 * @brief    StartAU writer-like interface used for the VSS 3GPP only
 * @note
 * @param    pContext: (IN) It is the VSS 3GPP context in our case
 * @param    streamID: (IN) Id of the stream to which the Access Unit is related.
 * @param    pAU:      (IN/OUT) Access Unit to be prepared.
 * @return    M4NO_ERROR: there is no error
 ******************************************************************************
 */
M4OSA_ERR M4VSS3GPP_intStartAU( M4WRITER_Context pContext,
                               M4SYS_StreamID streamID, M4SYS_AccessUnit *pAU )
{
    M4OSA_ERR err;
    M4OSA_UInt32 uiMaxAuSize;

    /**
    * Given context is actually the VSS3GPP context */
    M4VSS3GPP_InternalEditContext *pC =
        (M4VSS3GPP_InternalEditContext *)pContext;

    /**
    * Get the output AU to write into */
    err = pC->ShellAPI.pWriterDataFcts->pStartAU(pC->ewc.p3gpWriterContext,
        M4VSS3GPP_WRITER_VIDEO_STREAM_ID, pAU);

    if( M4NO_ERROR != err )
    {
        M4OSA_TRACE1_1(
            "M4VSS3GPP_intStartAU: pWriterDataFcts->pStartAU(Video) returns 0x%x!",
            err);
        return err;
    }

    /**
    *    Return */
    M4OSA_TRACE3_0("M4VSS3GPP_intStartAU: returning M4NO_ERROR");
    return M4NO_ERROR;
}

/******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intProcessAU()
 * @brief    ProcessAU writer-like interface used for the VSS 3GPP only
 * @note
 * @param    pContext: (IN) It is the VSS 3GPP context in our case
 * @param    streamID: (IN) Id of the stream to which the Access Unit is related.
 * @param    pAU:      (IN/OUT) Access Unit to be written
 * @return    M4NO_ERROR: there is no error
 ******************************************************************************
 */
M4OSA_ERR M4VSS3GPP_intProcessAU( M4WRITER_Context pContext,
                                 M4SYS_StreamID streamID, M4SYS_AccessUnit *pAU )
{
    M4OSA_ERR err;

    /**
    * Given context is actually the VSS3GPP context */
    M4VSS3GPP_InternalEditContext *pC =
        (M4VSS3GPP_InternalEditContext *)pContext;

    /**
    * Fix the encoded AU time */
    // Decorrelate input and output encoding timestamp to handle encoder prefetch
    pC->ewc.dOutputVidCts = pAU->CTS;
    /**
    * Update time info for the Counter Time System to be equal to the bit-stream time */
    M4VSS3GPP_intUpdateTimeInfo(pC, pAU);

    /**
    * Write the AU */
    err = pC->ShellAPI.pWriterDataFcts->pProcessAU(pC->ewc.p3gpWriterContext,
        M4VSS3GPP_WRITER_VIDEO_STREAM_ID, pAU);

    if( M4NO_ERROR != err )
    {
        M4OSA_TRACE1_1(
            "M4VSS3GPP_intProcessAU: pWriterDataFcts->pProcessAU(Video) returns 0x%x!",
            err);
        return err;
    }

    /**
    *    Return */
    M4OSA_TRACE3_0("M4VSS3GPP_intProcessAU: returning M4NO_ERROR");
    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intVPP()
 * @brief    We implement our own VideoPreProcessing function
 * @note    It is called by the video encoder
 * @param    pContext    (IN) VPP context, which actually is the VSS 3GPP context in our case
 * @param    pPlaneIn    (IN)
 * @param    pPlaneOut    (IN/OUT) Pointer to an array of 3 planes that will contain the output
 *                                  YUV420 image
 * @return    M4NO_ERROR:    No error
 ******************************************************************************
 */
M4OSA_ERR M4VSS3GPP_intVPP( M4VPP_Context pContext, M4VIFI_ImagePlane *pPlaneIn,
                           M4VIFI_ImagePlane *pPlaneOut )
{
    M4OSA_ERR err;
    M4_MediaTime t;
    M4VIFI_ImagePlane *pTmp = M4OSA_NULL;
    M4VIFI_ImagePlane pTemp1[3],pTemp2[3];
    M4OSA_UInt32  i =0;
    /**
    * VPP context is actually the VSS3GPP context */
    M4VSS3GPP_InternalEditContext *pC =
        (M4VSS3GPP_InternalEditContext *)pContext;
    pTemp1[0].pac_data = pTemp2[0].pac_data = M4OSA_NULL;
    /**
    * Reset VPP error remembered in context */
    pC->ewc.VppError = M4NO_ERROR;

    /**
    * At the end of the editing, we may be called when no more clip is loaded.
    * (because to close the encoder properly it must be stepped one or twice...) */
    if( M4OSA_NULL == pC->pC1 )
    {
        /**
        * We must fill the input of the encoder with a dummy image, because
        * encoding noise leads to a huge video AU, and thus a writer buffer overflow. */
        memset((void *)pPlaneOut[0].pac_data,0,
            pPlaneOut[0].u_stride * pPlaneOut[0].u_height);
        memset((void *)pPlaneOut[1].pac_data,0,
            pPlaneOut[1].u_stride * pPlaneOut[1].u_height);
        memset((void *)pPlaneOut[2].pac_data,0,
            pPlaneOut[2].u_stride * pPlaneOut[2].u_height);

        M4OSA_TRACE3_0("M4VSS3GPP_intVPP: returning M4NO_ERROR (abort)");
        return M4NO_ERROR;
    }

    /**
    **************** Transition case ****************/
    if( M4OSA_TRUE == pC->bTransitionEffect )
    {
        if (M4OSA_NULL == pTemp1[0].pac_data)
        {
            err = M4VSS3GPP_intAllocateYUV420(pTemp1, pC->ewc.uiVideoWidth,
                                              pC->ewc.uiVideoHeight);
            if (M4NO_ERROR != err)
            {
                M4OSA_TRACE1_1("M4VSS3GPP_intVPP: M4VSS3GPP_intAllocateYUV420(1) returns 0x%x, \
                               returning M4NO_ERROR", err);
                pC->ewc.VppError = err;
                return M4NO_ERROR; /**< Return no error to the encoder core
                                   (else it may leak in some situations...) */
            }
        }
        if (M4OSA_NULL == pTemp2[0].pac_data)
        {
            err = M4VSS3GPP_intAllocateYUV420(pTemp2, pC->ewc.uiVideoWidth,
                                              pC->ewc.uiVideoHeight);
            if (M4NO_ERROR != err)
            {
                M4OSA_TRACE1_1("M4VSS3GPP_intVPP: M4VSS3GPP_intAllocateYUV420(2) returns 0x%x, \
                               returning M4NO_ERROR", err);
                pC->ewc.VppError = err;
                return M4NO_ERROR; /**< Return no error to the encoder core
                                  (else it may leak in some situations...) */
            }
        }
        /**
        * We need two intermediate planes */
        if( M4OSA_NULL == pC->yuv1[0].pac_data )
        {
            err = M4VSS3GPP_intAllocateYUV420(pC->yuv1, pC->ewc.uiVideoWidth,
                pC->ewc.uiVideoHeight);

            if( M4NO_ERROR != err )
            {
                M4OSA_TRACE1_1(
                    "M4VSS3GPP_intVPP: M4VSS3GPP_intAllocateYUV420(3) returns 0x%x,\
                    returning M4NO_ERROR",
                    err);
                pC->ewc.VppError = err;
                return
                    M4NO_ERROR; /**< Return no error to the encoder core
                                (else it may leak in some situations...) */
            }
        }

        if( M4OSA_NULL == pC->yuv2[0].pac_data )
        {
            err = M4VSS3GPP_intAllocateYUV420(pC->yuv2, pC->ewc.uiVideoWidth,
                pC->ewc.uiVideoHeight);

            if( M4NO_ERROR != err )
            {
                M4OSA_TRACE1_1(
                    "M4VSS3GPP_intVPP: M4VSS3GPP_intAllocateYUV420(4) returns 0x%x,\
                    returning M4NO_ERROR",
                    err);
                pC->ewc.VppError = err;
                return
                    M4NO_ERROR; /**< Return no error to the encoder core
                                (else it may leak in some situations...) */
            }
        }

        /**
        * Allocate new temporary plane if needed */
        if( M4OSA_NULL == pC->yuv3[0].pac_data )
        {
            err = M4VSS3GPP_intAllocateYUV420(pC->yuv3, pC->ewc.uiVideoWidth,
                pC->ewc.uiVideoHeight);

            if( M4NO_ERROR != err )
            {
                M4OSA_TRACE1_1(
                    "M4VSS3GPP_intVPP: M4VSS3GPP_intAllocateYUV420(3) returns 0x%x,\
                    returning M4NO_ERROR",
                    err);
                pC->ewc.VppError = err;
                return
                    M4NO_ERROR; /**< Return no error to the encoder core
                                (else it may leak in some situations...) */
            }
        }

        /**
        * Compute the time in the clip1 base: t = to - Offset */
        // Decorrelate input and output encoding timestamp to handle encoder prefetch
        t = pC->ewc.dInputVidCts - pC->pC1->iVoffset;

        /**
        * Render Clip1 */
        if( pC->pC1->isRenderDup == M4OSA_FALSE )
        {
            if(pC->nbActiveEffects > 0)
            {
                err = pC->pC1->ShellAPI.m_pVideoDecoder->m_pFctRender(pC->pC1->pViDecCtxt,
                                                                      &t, pTemp1,
                                                                      M4OSA_TRUE);
                if (M4NO_ERROR != err)
                {
                    M4OSA_TRACE1_1("M4VSS3GPP_intVPP: m_pVideoDecoder->m_pFctRender(C1) returns 0x%x, \
                                    returning M4NO_ERROR", err);
                    pC->ewc.VppError = err;
                    return M4NO_ERROR; /**< Return no error to the encoder core
                                       (else it may leak in some situations...) */
                }
                pC->bIssecondClip = M4OSA_FALSE;
                err = M4VSS3GPP_intApplyVideoEffect(pC, pTemp1 ,pC->yuv1 );
                if (M4NO_ERROR != err)
                {
                    M4OSA_TRACE1_1("M4VSS3GPP_intVPP: M4VSS3GPP_intApplyVideoEffect(1) returns 0x%x, \
                                    returning M4NO_ERROR", err);
                    pC->ewc.VppError = err;
                    return M4NO_ERROR; /**< Return no error to the encoder core
                                       (else it may leak in some situations...) */
                }
                pC->pC1->lastDecodedPlane = pTemp1;
            }
            else
            {
                err = pC->pC1->ShellAPI.m_pVideoDecoder->m_pFctRender(pC->pC1->pViDecCtxt,
                                                                      &t, pC->yuv1,
                                                                      M4OSA_TRUE);
                if (M4NO_ERROR != err)
                {
                    M4OSA_TRACE1_1("M4VSS3GPP_intVPP: m_pVideoDecoder->m_pFctRender(C1) returns 0x%x, \
                                    returning M4NO_ERROR", err);
                    pC->ewc.VppError = err;
                    return M4NO_ERROR; /**< Return no error to the encoder core
                                      (else it may leak in some situations...) */
                }
                pC->pC1->lastDecodedPlane = pC->yuv1;
            }
            pC->pC1->iVideoRenderCts = (M4OSA_Int32)t;
        }
        else
        {
            /* Copy last decoded plane to output plane */
            memcpy((void *)pTmp[0].pac_data,
                (void *)pC->pC1->lastDecodedPlane[0].pac_data,
                (pTmp[0].u_height * pTmp[0].u_width));
            memcpy((void *)pTmp[1].pac_data,
                (void *)pC->pC1->lastDecodedPlane[1].pac_data,
                (pTmp[1].u_height * pTmp[1].u_width));
            memcpy((void *)pTmp[2].pac_data,
                (void *)pC->pC1->lastDecodedPlane[2].pac_data,
                (pTmp[2].u_height * pTmp[2].u_width));
            pC->pC1->lastDecodedPlane = pTmp;
        }

        /**
        * Compute the time in the clip2 base: t = to - Offset */
        // Decorrelate input and output encoding timestamp to handle encoder prefetch
        t = pC->ewc.dInputVidCts - pC->pC2->iVoffset;
        /**
        * Render Clip2 */
        if( pC->pC2->isRenderDup == M4OSA_FALSE )
        {
            if(pC->nbActiveEffects1 > 0)
            {
                err = pC->pC2->ShellAPI.m_pVideoDecoder->m_pFctRender(pC->pC2->pViDecCtxt,
                                                                      &t, pTemp2,
                                                                      M4OSA_TRUE);
                if (M4NO_ERROR != err)
                {
                    M4OSA_TRACE1_1("M4VSS3GPP_intVPP: m_pVideoDecoder->m_pFctRender(C2) returns 0x%x, \
                                   returning M4NO_ERROR", err);
                    pC->ewc.VppError = err;
                    return M4NO_ERROR; /**< Return no error to the encoder core
                                       (else it may leak in some situations...) */
                }

                pC->bIssecondClip = M4OSA_TRUE;
                err = M4VSS3GPP_intApplyVideoEffect(pC, pTemp2 ,pC->yuv2);
                if (M4NO_ERROR != err)
                {
                    M4OSA_TRACE1_1("M4VSS3GPP_intVPP: M4VSS3GPP_intApplyVideoEffect(1) returns 0x%x, \
                                    returning M4NO_ERROR", err);
                    pC->ewc.VppError = err;
                    return M4NO_ERROR; /**< Return no error to the encoder core
                                       (else it may leak in some situations...) */
                }
                pC->pC2->lastDecodedPlane = pTemp2;
            }
            else
            {
                err = pC->pC2->ShellAPI.m_pVideoDecoder->m_pFctRender(pC->pC2->pViDecCtxt,
                                                                      &t, pC->yuv2,
                                                                      M4OSA_TRUE);
                if (M4NO_ERROR != err)
                {
                    M4OSA_TRACE1_1("M4VSS3GPP_intVPP: m_pVideoDecoder->m_pFctRender(C2) returns 0x%x, \
                                    returning M4NO_ERROR", err);
                    pC->ewc.VppError = err;
                    return M4NO_ERROR; /**< Return no error to the encoder core
                                       (else it may leak in some situations...) */
                }
                pC->pC2->lastDecodedPlane = pC->yuv2;
            }
            pC->pC2->iVideoRenderCts = (M4OSA_Int32)t;
        }
        else
        {
            /* Copy last decoded plane to output plane */
            memcpy((void *)pTmp[0].pac_data,
                (void *)pC->pC2->lastDecodedPlane[0].pac_data,
                (pTmp[0].u_height * pTmp[0].u_width));
            memcpy((void *)pTmp[1].pac_data,
                (void *)pC->pC2->lastDecodedPlane[1].pac_data,
                (pTmp[1].u_height * pTmp[1].u_width));
            memcpy((void *)pTmp[2].pac_data,
                (void *)pC->pC2->lastDecodedPlane[2].pac_data,
                (pTmp[2].u_height * pTmp[2].u_width));
            pC->pC2->lastDecodedPlane = pTmp;
        }


        pTmp = pPlaneOut;
        err = M4VSS3GPP_intVideoTransition(pC, pTmp);

        if( M4NO_ERROR != err )
        {
            M4OSA_TRACE1_1(
                "M4VSS3GPP_intVPP: M4VSS3GPP_intVideoTransition returns 0x%x,\
                returning M4NO_ERROR",
                err);
            pC->ewc.VppError = err;
            return  M4NO_ERROR; /**< Return no error to the encoder core
                                (else it may leak in some situations...) */
        }
        for (i=0; i < 3; i++)
        {
            if (pTemp2[i].pac_data != M4OSA_NULL)
            {
                free(pTemp2[i].pac_data);
                pTemp2[i].pac_data = M4OSA_NULL;
            }


            if (pTemp1[i].pac_data != M4OSA_NULL)
            {
                    free(pTemp1[i].pac_data);
                    pTemp1[i].pac_data = M4OSA_NULL;
                }
            }
    }
    /**
    **************** No Transition case ****************/
    else
    {
        /**
        * Check if there is a filter */
        if( pC->nbActiveEffects > 0 )
        {
            /**
            * If we do modify the image, we need an intermediate image plane */
            if( M4OSA_NULL == pC->yuv1[0].pac_data )
            {
                err =
                    M4VSS3GPP_intAllocateYUV420(pC->yuv1, pC->ewc.uiVideoWidth,
                    pC->ewc.uiVideoHeight);

                if( M4NO_ERROR != err )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intVPP: M4VSS3GPP_intAllocateYUV420 returns 0x%x,\
                        returning M4NO_ERROR",
                        err);
                    pC->ewc.VppError = err;
                    return
                        M4NO_ERROR; /**< Return no error to the encoder core
                                    (else it may leak in some situations...) */
                }
            }
            /**
            * The image is rendered in the intermediate image plane */
            pTmp = pC->yuv1;
        }
        else
        {
            /**
            * No filter, the image is directly rendered in pPlaneOut */
            pTmp = pPlaneOut;
        }

        /**
        * Compute the time in the clip base: t = to - Offset */
        // Decorrelate input and output encoding timestamp to handle encoder prefetch
        t = pC->ewc.dInputVidCts - pC->pC1->iVoffset;

        if( pC->pC1->isRenderDup == M4OSA_FALSE )
        {
            err = pC->pC1->ShellAPI.m_pVideoDecoder->m_pFctRender(
                pC->pC1->pViDecCtxt, &t, pTmp, M4OSA_TRUE);

            if( M4NO_ERROR != err )
            {
                M4OSA_TRACE1_1(
                    "M4VSS3GPP_intVPP: m_pVideoDecoder->m_pFctRender returns 0x%x,\
                    returning M4NO_ERROR",
                    err);
                pC->ewc.VppError = err;
                return
                    M4NO_ERROR; /**< Return no error to the encoder core
                                (else it may leak in some situations...) */
            }
            pC->pC1->lastDecodedPlane = pTmp;
            pC->pC1->iVideoRenderCts = (M4OSA_Int32)t;
        }
        else
        {
            /* Copy last decoded plane to output plane */
            memcpy((void *)pTmp[0].pac_data,
                (void *)pC->pC1->lastDecodedPlane[0].pac_data,
                (pTmp[0].u_height * pTmp[0].u_width));
            memcpy((void *)pTmp[1].pac_data,
                (void *)pC->pC1->lastDecodedPlane[1].pac_data,
                (pTmp[1].u_height * pTmp[1].u_width));
            memcpy((void *)pTmp[2].pac_data,
                (void *)pC->pC1->lastDecodedPlane[2].pac_data,
                (pTmp[2].u_height * pTmp[2].u_width));
            pC->pC1->lastDecodedPlane = pTmp;
        }

        M4OSA_TRACE3_1("M4VSS3GPP_intVPP: Rendered at CTS %.3f", t);

        /**
        * Apply the clip1 effect */
        //        if (pC->iClip1ActiveEffect >= 0)
        if( pC->nbActiveEffects > 0 )
        {
            err = M4VSS3GPP_intApplyVideoEffect(pC,/*1,*/ pC->yuv1, pPlaneOut);

            if( M4NO_ERROR != err )
            {
                M4OSA_TRACE1_1(
                    "M4VSS3GPP_intVPP: M4VSS3GPP_intApplyVideoEffect(1) returns 0x%x,\
                    returning M4NO_ERROR",
                    err);
                pC->ewc.VppError = err;
                return
                    M4NO_ERROR; /**< Return no error to the encoder core
                                (else it may leak in some situations...) */
            }
        }
    }

    /**
    *    Return */
    M4OSA_TRACE3_0("M4VSS3GPP_intVPP: returning M4NO_ERROR");
    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intApplyVideoEffect()
 * @brief    Apply video effect from pPlaneIn to pPlaneOut
 * @param   pC                (IN/OUT) Internal edit context
 * @param   uiClip1orClip2    (IN/OUT) 1 for first clip, 2 for second clip
 * @param    pInputPlanes    (IN) Input raw YUV420 image
 * @param    pOutputPlanes    (IN/OUT) Output raw YUV420 image
 * @return    M4NO_ERROR:                        No error
 ******************************************************************************
 */
static M4OSA_ERR
M4VSS3GPP_intApplyVideoEffect( M4VSS3GPP_InternalEditContext *pC,
                               M4VIFI_ImagePlane *pPlaneIn,
                               M4VIFI_ImagePlane *pPlaneOut )
{
    M4OSA_ERR err;

    M4VSS3GPP_ClipContext *pClip;
    M4VSS3GPP_EffectSettings *pFx;
    M4VFL_CurtainParam curtainParams;
    M4VSS3GPP_ExternalProgress extProgress;

    M4OSA_Double VideoEffectTime;
    M4OSA_Double PercentageDone;
    M4OSA_Int32 tmp;

    M4VIFI_ImagePlane *pPlaneTempIn;
    M4VIFI_ImagePlane *pPlaneTempOut;
    M4OSA_UInt8 i;
    M4OSA_UInt8 NumActiveEffects =0;


    pClip = pC->pC1;
    if (pC->bIssecondClip == M4OSA_TRUE)
    {
        NumActiveEffects = pC->nbActiveEffects1;
    }
    else
    {
        NumActiveEffects = pC->nbActiveEffects;
    }

    /**
    * Allocate temporary plane if needed RC */
    if (M4OSA_NULL == pC->yuv4[0].pac_data && NumActiveEffects  > 1)
    {
        err = M4VSS3GPP_intAllocateYUV420(pC->yuv4, pC->ewc.uiVideoWidth,
            pC->ewc.uiVideoHeight);

        if( M4NO_ERROR != err )
        {
            M4OSA_TRACE1_1(
                "M4VSS3GPP_intApplyVideoEffect: M4VSS3GPP_intAllocateYUV420(4) returns 0x%x,\
                returning M4NO_ERROR",
                err);
            pC->ewc.VppError = err;
            return
                M4NO_ERROR; /**< Return no error to the encoder core
                            (else it may leak in some situations...) */
        }
    }

    if (NumActiveEffects  % 2 == 0)
    {
        pPlaneTempIn = pPlaneIn;
        pPlaneTempOut = pC->yuv4;
    }
    else
    {
        pPlaneTempIn = pPlaneIn;
        pPlaneTempOut = pPlaneOut;
    }

    for (i=0; i<NumActiveEffects; i++)
    {
        if (pC->bIssecondClip == M4OSA_TRUE)
        {


            pFx = &(pC->pEffectsList[pC->pActiveEffectsList1[i]]);
            /* Compute how far from the beginning of the effect we are, in clip-base time. */
            // Decorrelate input and output encoding timestamp to handle encoder prefetch
            VideoEffectTime = ((M4OSA_Int32)pC->ewc.dInputVidCts) +
                              pC->pTransitionList[pC->uiCurrentClip].
                              uiTransitionDuration- pFx->uiStartTime;
        }
        else
        {
            pFx = &(pC->pEffectsList[pC->pActiveEffectsList[i]]);
            /* Compute how far from the beginning of the effect we are, in clip-base time. */
            // Decorrelate input and output encoding timestamp to handle encoder prefetch
            VideoEffectTime = ((M4OSA_Int32)pC->ewc.dInputVidCts) - pFx->uiStartTime;
        }



        /* To calculate %, substract timeIncrement because effect should finish on the last frame*/
        /* which is presented from CTS = eof-timeIncrement till CTS = eof */
        PercentageDone = VideoEffectTime
            / ((M4OSA_Float)pFx->uiDuration/*- pC->dOutputFrameDuration*/);

        if( PercentageDone < 0.0 )
            PercentageDone = 0.0;

        if( PercentageDone > 1.0 )
            PercentageDone = 1.0;

        switch( pFx->VideoEffectType )
        {
            case M4VSS3GPP_kVideoEffectType_FadeFromBlack:
                /**
                * Compute where we are in the effect (scale is 0->1024). */
                tmp = (M4OSA_Int32)(PercentageDone * 1024);

                /**
                * Apply the darkening effect */
                err =
                    M4VFL_modifyLumaWithScale((M4ViComImagePlane *)pPlaneTempIn,
                    (M4ViComImagePlane *)pPlaneTempOut, tmp, M4OSA_NULL);

                if( M4NO_ERROR != err )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intApplyVideoEffect:\
                        M4VFL_modifyLumaWithScale returns error 0x%x,\
                        returning M4VSS3GPP_ERR_LUMA_FILTER_ERROR",
                        err);
                    return M4VSS3GPP_ERR_LUMA_FILTER_ERROR;
                }
                break;

            case M4VSS3GPP_kVideoEffectType_CurtainOpening:
                /**
                * Compute where we are in the effect (scale is 0->height).
                * It is done with floats because tmp x height can be very large
                (with long clips).*/
                curtainParams.nb_black_lines =
                    (M4OSA_UInt16)(( 1.0 - PercentageDone)
                    * pPlaneTempIn[0].u_height);
                /**
                * The curtain is hanged on the ceiling */
                curtainParams.top_is_black = 1;

                /**
                * Apply the curtain effect */
                err = M4VFL_applyCurtain((M4ViComImagePlane *)pPlaneTempIn,
                    (M4ViComImagePlane *)pPlaneTempOut, &curtainParams,
                    M4OSA_NULL);

                if( M4NO_ERROR != err )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intApplyVideoEffect: M4VFL_applyCurtain returns error 0x%x,\
                        returning M4VSS3GPP_ERR_CURTAIN_FILTER_ERROR",
                        err);
                    return M4VSS3GPP_ERR_CURTAIN_FILTER_ERROR;
                }
                break;

            case M4VSS3GPP_kVideoEffectType_FadeToBlack:
                /**
                * Compute where we are in the effect (scale is 0->1024) */
                tmp = (M4OSA_Int32)(( 1.0 - PercentageDone) * 1024);

                /**
                * Apply the darkening effect */
                err =
                    M4VFL_modifyLumaWithScale((M4ViComImagePlane *)pPlaneTempIn,
                    (M4ViComImagePlane *)pPlaneTempOut, tmp, M4OSA_NULL);

                if( M4NO_ERROR != err )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intApplyVideoEffect:\
                        M4VFL_modifyLumaWithScale returns error 0x%x,\
                        returning M4VSS3GPP_ERR_LUMA_FILTER_ERROR",
                        err);
                    return M4VSS3GPP_ERR_LUMA_FILTER_ERROR;
                }
                break;

            case M4VSS3GPP_kVideoEffectType_CurtainClosing:
                /**
                * Compute where we are in the effect (scale is 0->height) */
                curtainParams.nb_black_lines =
                    (M4OSA_UInt16)(PercentageDone * pPlaneTempIn[0].u_height);

                /**
                * The curtain is hanged on the ceiling */
                curtainParams.top_is_black = 1;

                /**
                * Apply the curtain effect */
                err = M4VFL_applyCurtain((M4ViComImagePlane *)pPlaneTempIn,
                    (M4ViComImagePlane *)pPlaneTempOut, &curtainParams,
                    M4OSA_NULL);

                if( M4NO_ERROR != err )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intApplyVideoEffect: M4VFL_applyCurtain returns error 0x%x,\
                        returning M4VSS3GPP_ERR_CURTAIN_FILTER_ERROR",
                        err);
                    return M4VSS3GPP_ERR_CURTAIN_FILTER_ERROR;
                }
                break;

            default:
                if( pFx->VideoEffectType
                    >= M4VSS3GPP_kVideoEffectType_External )
                {
                    M4OSA_UInt32 Cts = 0;
                    M4OSA_Int32 nextEffectTime;

                    /**
                    * Compute where we are in the effect (scale is 0->1000) */
                    tmp = (M4OSA_Int32)(PercentageDone * 1000);

                    /**
                    * Set the progress info provided to the external function */
                    extProgress.uiProgress = (M4OSA_UInt32)tmp;
                    // Decorrelate input and output encoding timestamp to handle encoder prefetch
                    extProgress.uiOutputTime = (M4OSA_UInt32)pC->ewc.dInputVidCts;
                    extProgress.uiClipTime = extProgress.uiOutputTime - pClip->iVoffset;
                    extProgress.bIsLast = M4OSA_FALSE;
                    // Decorrelate input and output encoding timestamp to handle encoder prefetch
                    nextEffectTime = (M4OSA_Int32)(pC->ewc.dInputVidCts \
                        + pC->dOutputFrameDuration);
                    if(nextEffectTime >= (M4OSA_Int32)(pFx->uiStartTime + pFx->uiDuration))
                    {
                        extProgress.bIsLast = M4OSA_TRUE;
                    }

                    err = pFx->ExtVideoEffectFct(pFx->pExtVideoEffectFctCtxt,
                        pPlaneTempIn, pPlaneTempOut, &extProgress,
                        pFx->VideoEffectType
                        - M4VSS3GPP_kVideoEffectType_External);

                    if( M4NO_ERROR != err )
                    {
                        M4OSA_TRACE1_1(
                            "M4VSS3GPP_intApplyVideoEffect: \
                            External video effect function returns 0x%x!",
                            err);
                        return err;
                    }
                    break;
                }
                else
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intApplyVideoEffect: unknown effect type (0x%x),\
                        returning M4VSS3GPP_ERR_INVALID_VIDEO_EFFECT_TYPE",
                        pFx->VideoEffectType);
                    return M4VSS3GPP_ERR_INVALID_VIDEO_EFFECT_TYPE;
                }
        }
        /**
        * RC Updates pTempPlaneIn and pTempPlaneOut depending on current effect */
        if (((i % 2 == 0) && (NumActiveEffects  % 2 == 0))
            || ((i % 2 != 0) && (NumActiveEffects % 2 != 0)))
        {
            pPlaneTempIn = pC->yuv4;
            pPlaneTempOut = pPlaneOut;
        }
        else
        {
            pPlaneTempIn = pPlaneOut;
            pPlaneTempOut = pC->yuv4;
        }
    }

    /**
    *    Return */
    M4OSA_TRACE3_0("M4VSS3GPP_intApplyVideoEffect: returning M4NO_ERROR");
    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intVideoTransition()
 * @brief    Apply video transition effect pC1+pC2->pPlaneOut
 * @param   pC                (IN/OUT) Internal edit context
 * @param    pOutputPlanes    (IN/OUT) Output raw YUV420 image
 * @return    M4NO_ERROR:                        No error
 ******************************************************************************
 */
static M4OSA_ERR
M4VSS3GPP_intVideoTransition( M4VSS3GPP_InternalEditContext *pC,
                             M4VIFI_ImagePlane *pPlaneOut )
{
    M4OSA_ERR err;
    M4OSA_Int32 iProgress;
    M4VSS3GPP_ExternalProgress extProgress;
    M4VIFI_ImagePlane *pPlane;
    M4OSA_Int32 i;
    const M4OSA_Int32 iDur = (M4OSA_Int32)pC->
        pTransitionList[pC->uiCurrentClip].uiTransitionDuration;

    /**
    * Compute how far from the end cut we are, in clip-base time.
    * It is done with integers because the offset and begin cut have been rounded already. */
    // Decorrelate input and output encoding timestamp to handle encoder prefetch
    iProgress = (M4OSA_Int32)((M4OSA_Double)pC->pC1->iEndTime) - pC->ewc.dInputVidCts +
        ((M4OSA_Double)pC->pC1->iVoffset);
    /**
    * We must remove the duration of one frame, else we would almost never reach the end
    * (It's kind of a "pile and intervals" issue). */
    iProgress -= (M4OSA_Int32)pC->dOutputFrameDuration;

    if( iProgress < 0 ) /**< Sanity checks */
    {
        iProgress = 0;
    }

    /**
    * Compute where we are in the transition, on a base 1000 */
    iProgress = ( ( iDur - iProgress) * 1000) / iDur;

    /**
    * Sanity checks */
    if( iProgress < 0 )
    {
        iProgress = 0;
    }
    else if( iProgress > 1000 )
    {
        iProgress = 1000;
    }

    switch( pC->pTransitionList[pC->uiCurrentClip].TransitionBehaviour )
    {
        case M4VSS3GPP_TransitionBehaviour_SpeedUp:
            iProgress = ( iProgress * iProgress) / 1000;
            break;

        case M4VSS3GPP_TransitionBehaviour_Linear:
            /*do nothing*/
            break;

        case M4VSS3GPP_TransitionBehaviour_SpeedDown:
            iProgress = (M4OSA_Int32)(sqrt(iProgress * 1000));
            break;

        case M4VSS3GPP_TransitionBehaviour_SlowMiddle:
            if( iProgress < 500 )
            {
                iProgress = (M4OSA_Int32)(sqrt(iProgress * 500));
            }
            else
            {
                iProgress =
                    (M4OSA_Int32)(( ( ( iProgress - 500) * (iProgress - 500))
                    / 500) + 500);
            }
            break;

        case M4VSS3GPP_TransitionBehaviour_FastMiddle:
            if( iProgress < 500 )
            {
                iProgress = (M4OSA_Int32)(( iProgress * iProgress) / 500);
            }
            else
            {
                iProgress = (M4OSA_Int32)(sqrt(( iProgress - 500) * 500) + 500);
            }
            break;

        default:
            /*do nothing*/
            break;
    }

    switch( pC->pTransitionList[pC->uiCurrentClip].VideoTransitionType )
    {
        case M4VSS3GPP_kVideoTransitionType_CrossFade:
            /**
            * Apply the transition effect */
            err = M4VIFI_ImageBlendingonYUV420(M4OSA_NULL,
                (M4ViComImagePlane *)pC->yuv1,
                (M4ViComImagePlane *)pC->yuv2,
                (M4ViComImagePlane *)pPlaneOut, iProgress);

            if( M4NO_ERROR != err )
            {
                M4OSA_TRACE1_1(
                    "M4VSS3GPP_intVideoTransition:\
                     M4VIFI_ImageBlendingonYUV420 returns error 0x%x,\
                    returning M4VSS3GPP_ERR_TRANSITION_FILTER_ERROR",
                    err);
                return M4VSS3GPP_ERR_TRANSITION_FILTER_ERROR;
            }
            break;

        case M4VSS3GPP_kVideoTransitionType_None:
            /**
            * This is a stupid-non optimized version of the None transition...
            * We copy the YUV frame */
            if( iProgress < 500 ) /**< first half of transition */
            {
                pPlane = pC->yuv1;
            }
            else /**< second half of transition */
            {
                pPlane = pC->yuv2;
            }
            /**
            * Copy the input YUV frames */
            i = 3;

            while( i-- > 0 )
            {
                memcpy((void *)pPlaneOut[i].pac_data,
                 (void *)pPlane[i].pac_data,
                    pPlaneOut[i].u_stride * pPlaneOut[i].u_height);
            }
            break;

        default:
            if( pC->pTransitionList[pC->uiCurrentClip].VideoTransitionType
                >= M4VSS3GPP_kVideoTransitionType_External )
            {
                /**
                * Set the progress info provided to the external function */
                extProgress.uiProgress = (M4OSA_UInt32)iProgress;
                // Decorrelate input and output encoding timestamp to handle encoder prefetch
                extProgress.uiOutputTime = (M4OSA_UInt32)pC->ewc.dInputVidCts;
                extProgress.uiClipTime = extProgress.uiOutputTime - pC->pC1->iVoffset;

                err = pC->pTransitionList[pC->
                    uiCurrentClip].ExtVideoTransitionFct(
                    pC->pTransitionList[pC->
                    uiCurrentClip].pExtVideoTransitionFctCtxt,
                    pC->yuv1, pC->yuv2, pPlaneOut, &extProgress,
                    pC->pTransitionList[pC->
                    uiCurrentClip].VideoTransitionType
                    - M4VSS3GPP_kVideoTransitionType_External);

                if( M4NO_ERROR != err )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intVideoTransition:\
                        External video transition function returns 0x%x!",
                        err);
                    return err;
                }
                break;
            }
            else
            {
                M4OSA_TRACE1_1(
                    "M4VSS3GPP_intVideoTransition: unknown transition type (0x%x),\
                    returning M4VSS3GPP_ERR_INVALID_VIDEO_TRANSITION_TYPE",
                    pC->pTransitionList[pC->uiCurrentClip].VideoTransitionType);
                return M4VSS3GPP_ERR_INVALID_VIDEO_TRANSITION_TYPE;
            }
    }

    /**
    *    Return */
    M4OSA_TRACE3_0("M4VSS3GPP_intVideoTransition: returning M4NO_ERROR");
    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * M4OSA_Void M4VSS3GPP_intUpdateTimeInfo()
 * @brief    Update bit stream time info by Counter Time System to be compliant with
 *          players using bit stream time info
 * @note    H263 uses an absolute time counter unlike MPEG4 which uses Group Of Vops
 *          (GOV, see the standard)
 * @param   pC                    (IN/OUT) returns time updated video AU,
 *                                the offset between system and video time (MPEG4 only)
 *                                and the state of the current clip (MPEG4 only)
 * @return    nothing
 ******************************************************************************
 */
static M4OSA_Void
M4VSS3GPP_intUpdateTimeInfo( M4VSS3GPP_InternalEditContext *pC,
                            M4SYS_AccessUnit *pAU )
{
    M4OSA_UInt8 uiTmp;
    M4OSA_UInt32 uiCts = 0;
    M4OSA_MemAddr8 pTmp;
    M4OSA_UInt32 uiAdd;
    M4OSA_UInt32 uiCurrGov;
    M4OSA_Int8 iDiff;

    M4VSS3GPP_ClipContext *pClipCtxt = pC->pC1;
    M4OSA_Int32 *pOffset = &(pC->ewc.iMpeg4GovOffset);

    /**
    * Set H263 time counter from system time */
    if( M4SYS_kH263 == pAU->stream->streamType )
    {
        uiTmp = (M4OSA_UInt8)((M4OSA_UInt32)( ( pAU->CTS * 30) / 1001 + 0.5)
            % M4VSS3GPP_EDIT_H263_MODULO_TIME);
        M4VSS3GPP_intSetH263TimeCounter((M4OSA_MemAddr8)(pAU->dataAddress),
            uiTmp);
    }
    /*
    * Set MPEG4 GOV time counter regarding video and system time */
    else if( M4SYS_kMPEG_4 == pAU->stream->streamType )
    {
        /*
        * If GOV.
        * beware of little/big endian! */
        /* correction: read 8 bits block instead of one 32 bits block */
        M4OSA_UInt8 *temp8 = (M4OSA_UInt8 *)(pAU->dataAddress);
        M4OSA_UInt32 temp32 = 0;

        temp32 = ( 0x000000ff & (M4OSA_UInt32)(*temp8))
            + (0x0000ff00 & ((M4OSA_UInt32)(*(temp8 + 1))) << 8)
            + (0x00ff0000 & ((M4OSA_UInt32)(*(temp8 + 2))) << 16)
            + (0xff000000 & ((M4OSA_UInt32)(*(temp8 + 3))) << 24);

        M4OSA_TRACE3_2("RC: Temp32: 0x%x, dataAddress: 0x%x\n", temp32,
            *(pAU->dataAddress));

        if( M4VSS3GPP_EDIT_GOV_HEADER == temp32 )
        {
            pTmp =
                (M4OSA_MemAddr8)(pAU->dataAddress
                + 1); /**< Jump to the time code (just after the 32 bits header) */
            uiAdd = (M4OSA_UInt32)(pAU->CTS)+( *pOffset);

            switch( pClipCtxt->bMpeg4GovState )
            {
                case M4OSA_FALSE: /*< INIT */
                    {
                        /* video time = ceil (system time + offset) */
                        uiCts = ( uiAdd + 999) / 1000;

                        /* offset update */
                        ( *pOffset) += (( uiCts * 1000) - uiAdd);

                        /* Save values */
                        pClipCtxt->uiMpeg4PrevGovValueSet = uiCts;

                        /* State to 'first' */
                        pClipCtxt->bMpeg4GovState = M4OSA_TRUE;
                    }
                    break;

                case M4OSA_TRUE: /*< UPDATE */
                    {
                        /* Get current Gov value */
                        M4VSS3GPP_intGetMPEG4Gov(pTmp, &uiCurrGov);

                        /* video time = floor or ceil (system time + offset) */
                        uiCts = (uiAdd / 1000);
                        iDiff = (M4OSA_Int8)(uiCurrGov
                            - pClipCtxt->uiMpeg4PrevGovValueGet - uiCts
                            + pClipCtxt->uiMpeg4PrevGovValueSet);

                        /* ceiling */
                        if( iDiff > 0 )
                        {
                            uiCts += (M4OSA_UInt32)(iDiff);

                            /* offset update */
                            ( *pOffset) += (( uiCts * 1000) - uiAdd);
                        }

                        /* Save values */
                        pClipCtxt->uiMpeg4PrevGovValueGet = uiCurrGov;
                        pClipCtxt->uiMpeg4PrevGovValueSet = uiCts;
                    }
                    break;
            }

            M4VSS3GPP_intSetMPEG4Gov(pTmp, uiCts);
        }
    }
    return;
}

/**
 ******************************************************************************
 * M4OSA_Void M4VSS3GPP_intCheckVideoEffects()
 * @brief    Check which video effect must be applied at the current time
 ******************************************************************************
 */
static M4OSA_Void
M4VSS3GPP_intCheckVideoEffects( M4VSS3GPP_InternalEditContext *pC,
                               M4OSA_UInt8 uiClipNumber )
{
    M4OSA_UInt8 uiClipIndex;
    M4OSA_UInt8 uiFxIndex, i;
    M4VSS3GPP_ClipContext *pClip;
    M4VSS3GPP_EffectSettings *pFx;
    M4OSA_Int32 Off, BC, EC;
    // Decorrelate input and output encoding timestamp to handle encoder prefetch
    M4OSA_Int32 t = (M4OSA_Int32)pC->ewc.dInputVidCts;

    uiClipIndex = pC->uiCurrentClip;
    pClip = pC->pC1;
    /**
    * Shortcuts for code readability */
    Off = pClip->iVoffset;
    BC = pClip->iActualVideoBeginCut;
    EC = pClip->iEndTime;

    i = 0;

    for ( uiFxIndex = 0; uiFxIndex < pC->nbEffects; uiFxIndex++ )
    {
        /** Shortcut, reverse order because of priority between effects(EndEffect always clean )*/
        pFx = &(pC->pEffectsList[pC->nbEffects - 1 - uiFxIndex]);

        if( M4VSS3GPP_kVideoEffectType_None != pFx->VideoEffectType )
        {
            /**
            * Check if there is actually a video effect */

             if(uiClipNumber ==1)
             {
                /**< Are we after the start time of the effect?
                 * or Are we into the effect duration?
                 */
                if ( (t >= (M4OSA_Int32)(pFx->uiStartTime)) &&
                    (t <= (M4OSA_Int32)(pFx->uiStartTime + pFx->uiDuration)) ) {
                    /**
                     * Set the active effect(s) */
                    pC->pActiveEffectsList[i] = pC->nbEffects-1-uiFxIndex;

                    /**
                     * Update counter of active effects */
                    i++;

                    /**
                     * For all external effects set this flag to true. */
                    if(pFx->VideoEffectType > M4VSS3GPP_kVideoEffectType_External)
                    {
                        pC->m_bClipExternalHasStarted = M4OSA_TRUE;
                    }

                    /**
                     * The third effect has the highest priority, then the
                     * second one, then the first one. Hence, as soon as we
                     * found an active effect, we can get out of this loop.
                     */
                }
            }
            else
            {
                /**< Are we into the effect duration? */
                if ( ((M4OSA_Int32)(t + pC->pTransitionList[uiClipIndex].uiTransitionDuration)
                    >= (M4OSA_Int32)(pFx->uiStartTime))
                    && ( (M4OSA_Int32)(t + pC->pTransitionList[uiClipIndex].uiTransitionDuration)
                    <= (M4OSA_Int32)(pFx->uiStartTime + pFx->uiDuration)) ) {
                    /**
                     * Set the active effect(s) */
                    pC->pActiveEffectsList1[i] = pC->nbEffects-1-uiFxIndex;

                    /**
                     * Update counter of active effects */
                    i++;

                    /**
                     * For all external effects set this flag to true. */
                    if(pFx->VideoEffectType > M4VSS3GPP_kVideoEffectType_External)
                    {
                        pC->m_bClipExternalHasStarted = M4OSA_TRUE;
                    }

                    /**
                     * The third effect has the highest priority, then the second one, then the first one.
                     * Hence, as soon as we found an active effect, we can get out of this loop */
                }
            }
        }
    }

    if(1==uiClipNumber)
    {
    /**
     * Save number of active effects */
        pC->nbActiveEffects = i;
    }
    else
    {
        pC->nbActiveEffects1 = i;
    }

    /**
    * Change the absolut time to clip related time */
    t -= Off;

    /**
    * Check if we are on the begin cut (for clip1 only) */
    if( ( 0 != BC) && (t == BC) && (1 == uiClipNumber) )
    {
        pC->bClip1AtBeginCut = M4OSA_TRUE;
    }
    else
    {
        pC->bClip1AtBeginCut = M4OSA_FALSE;
    }

    return;
}

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intCreateVideoEncoder()
 * @brief    Creates the video encoder
 * @note
 ******************************************************************************
 */
M4OSA_ERR M4VSS3GPP_intCreateVideoEncoder( M4VSS3GPP_InternalEditContext *pC )
{
    M4OSA_ERR err;
    M4ENCODER_AdvancedParams EncParams;

    /**
    * Simulate a writer interface with our specific function */
    pC->ewc.OurWriterDataInterface.pProcessAU =
        M4VSS3GPP_intProcessAU; /**< This function is VSS 3GPP specific,
                                but it follow the writer interface */
    pC->ewc.OurWriterDataInterface.pStartAU =
        M4VSS3GPP_intStartAU; /**< This function is VSS 3GPP specific,
                              but it follow the writer interface */
    pC->ewc.OurWriterDataInterface.pWriterContext =
        (M4WRITER_Context)
        pC; /**< We give the internal context as writer context */

    /**
    * Get the encoder interface, if not already done */
    if( M4OSA_NULL == pC->ShellAPI.pVideoEncoderGlobalFcts )
    {
        err = M4VSS3GPP_setCurrentVideoEncoder(&pC->ShellAPI,
            pC->ewc.VideoStreamType);
        M4OSA_TRACE1_1(
            "M4VSS3GPP_intCreateVideoEncoder: setCurrentEncoder returns 0x%x",
            err);
        M4ERR_CHECK_RETURN(err);
    }

    /**
    * Set encoder shell parameters according to VSS settings */

    /* Common parameters */
    EncParams.InputFormat = M4ENCODER_kIYUV420;
    EncParams.FrameWidth = pC->ewc.uiVideoWidth;
    EncParams.FrameHeight = pC->ewc.uiVideoHeight;
    EncParams.uiTimeScale = pC->ewc.uiVideoTimeScale;

    if( pC->bIsMMS == M4OSA_FALSE )
    {
        /* No strict regulation in video editor */
        /* Because of the effects and transitions we should allow more flexibility */
        /* Also it prevents to drop important frames (with a bad result on sheduling and
        block effetcs) */
        EncParams.bInternalRegulation = M4OSA_FALSE;
        // Variable framerate is not supported by StageFright encoders
        EncParams.FrameRate = M4ENCODER_k30_FPS;
    }
    else
    {
        /* In case of MMS mode, we need to enable bitrate regulation to be sure */
        /* to reach the targeted output file size */
        EncParams.bInternalRegulation = M4OSA_TRUE;
        EncParams.FrameRate = pC->MMSvideoFramerate;
    }

    /**
    * Other encoder settings (defaults) */
    EncParams.uiHorizontalSearchRange = 0;     /* use default */
    EncParams.uiVerticalSearchRange = 0;       /* use default */
    EncParams.bErrorResilience = M4OSA_FALSE;  /* no error resilience */
    EncParams.uiIVopPeriod = 0;                /* use default */
    EncParams.uiMotionEstimationTools = 0;     /* M4V_MOTION_EST_TOOLS_ALL */
    EncParams.bAcPrediction = M4OSA_TRUE;      /* use AC prediction */
    EncParams.uiStartingQuantizerValue = 10;   /* initial QP = 10 */
    EncParams.bDataPartitioning = M4OSA_FALSE; /* no data partitioning */

    switch ( pC->ewc.VideoStreamType )
    {
        case M4SYS_kH263:

            EncParams.Format = M4ENCODER_kH263;

            EncParams.uiStartingQuantizerValue = 10;
            EncParams.uiRateFactor = 1; /* default */

            EncParams.bErrorResilience = M4OSA_FALSE;
            EncParams.bDataPartitioning = M4OSA_FALSE;
            break;

        case M4SYS_kMPEG_4:

            EncParams.Format = M4ENCODER_kMPEG4;

            EncParams.uiStartingQuantizerValue = 8;
            EncParams.uiRateFactor = (M4OSA_UInt8)(( pC->dOutputFrameDuration
                * pC->ewc.uiVideoTimeScale) / 1000.0 + 0.5);

            if( EncParams.uiRateFactor == 0 )
                EncParams.uiRateFactor = 1; /* default */

            if( M4OSA_FALSE == pC->ewc.bVideoDataPartitioning )
            {
                EncParams.bErrorResilience = M4OSA_FALSE;
                EncParams.bDataPartitioning = M4OSA_FALSE;
            }
            else
            {
                EncParams.bErrorResilience = M4OSA_TRUE;
                EncParams.bDataPartitioning = M4OSA_TRUE;
            }
            break;

        case M4SYS_kH264:
            M4OSA_TRACE1_0("M4VSS3GPP_intCreateVideoEncoder: M4SYS_H264");

            EncParams.Format = M4ENCODER_kH264;

            EncParams.uiStartingQuantizerValue = 10;
            EncParams.uiRateFactor = 1; /* default */

            EncParams.bErrorResilience = M4OSA_FALSE;
            EncParams.bDataPartitioning = M4OSA_FALSE;
            //EncParams.FrameRate = M4VIDEOEDITING_k5_FPS;
            break;

        default:
            M4OSA_TRACE1_1(
                "M4VSS3GPP_intCreateVideoEncoder: Unknown videoStreamType 0x%x",
                pC->ewc.VideoStreamType);
            return M4VSS3GPP_ERR_EDITING_UNSUPPORTED_VIDEO_FORMAT;
    }

    /* In case of EMP we overwrite certain parameters */
    if( M4OSA_TRUE == pC->ewc.bActivateEmp )
    {
        EncParams.uiHorizontalSearchRange = 15;    /* set value */
        EncParams.uiVerticalSearchRange = 15;      /* set value */
        EncParams.bErrorResilience = M4OSA_FALSE;  /* no error resilience */
        EncParams.uiIVopPeriod = 15; /* one I frame every 15 frames */
        EncParams.uiMotionEstimationTools = 1; /* M4V_MOTION_EST_TOOLS_NO_4MV */
        EncParams.bAcPrediction = M4OSA_FALSE;     /* no AC prediction */
        EncParams.uiStartingQuantizerValue = 10;   /* initial QP = 10 */
        EncParams.bDataPartitioning = M4OSA_FALSE; /* no data partitioning */
    }

    if( pC->bIsMMS == M4OSA_FALSE )
    {
        /* Compute max bitrate depending on input files bitrates and transitions */
        if( pC->Vstate == M4VSS3GPP_kEditVideoState_TRANSITION )
        {
#if 0
            /* Max of the two blended files */
            if( pC->pC1->pSettings->ClipProperties.uiVideoBitrate
                > pC->pC2->pSettings->ClipProperties.uiVideoBitrate )
                EncParams.Bitrate =
                pC->pC1->pSettings->ClipProperties.uiVideoBitrate;
            else
                EncParams.Bitrate =
                pC->pC2->pSettings->ClipProperties.uiVideoBitrate;
#endif
            EncParams.Bitrate = pC->ewc.uiVideoBitrate;
        }
        else
        {
            EncParams.Bitrate = pC->ewc.uiVideoBitrate;
        }
    }
    else
    {
        EncParams.Bitrate = pC->uiMMSVideoBitrate; /* RC */
        EncParams.uiTimeScale = 0; /* We let the encoder choose the timescale */
    }

    M4OSA_TRACE1_0("M4VSS3GPP_intCreateVideoEncoder: calling encoder pFctInit");
    /**
    * Init the video encoder (advanced settings version of the encoder Open function) */
    err = pC->ShellAPI.pVideoEncoderGlobalFcts->pFctInit(&pC->ewc.pEncContext,
        &pC->ewc.OurWriterDataInterface, M4VSS3GPP_intVPP, pC,
        pC->ShellAPI.pCurrentVideoEncoderExternalAPI,
        pC->ShellAPI.pCurrentVideoEncoderUserData);

    if( M4NO_ERROR != err )
    {
        M4OSA_TRACE1_1(
            "M4VSS3GPP_intCreateVideoEncoder: pVideoEncoderGlobalFcts->pFctInit returns 0x%x",
            err);
        return err;
    }

    pC->ewc.encoderState = M4VSS3GPP_kEncoderClosed;
    M4OSA_TRACE1_0("M4VSS3GPP_intCreateVideoEncoder: calling encoder pFctOpen");

    err = pC->ShellAPI.pVideoEncoderGlobalFcts->pFctOpen(pC->ewc.pEncContext,
        &pC->ewc.WriterVideoAU, &EncParams);

    if( M4NO_ERROR != err )
    {
        M4OSA_TRACE1_1(
            "M4VSS3GPP_intCreateVideoEncoder: pVideoEncoderGlobalFcts->pFctOpen returns 0x%x",
            err);
        return err;
    }

    pC->ewc.encoderState = M4VSS3GPP_kEncoderStopped;
    M4OSA_TRACE1_0(
        "M4VSS3GPP_intCreateVideoEncoder: calling encoder pFctStart");

    if( M4OSA_NULL != pC->ShellAPI.pVideoEncoderGlobalFcts->pFctStart )
    {
        err = pC->ShellAPI.pVideoEncoderGlobalFcts->pFctStart(
            pC->ewc.pEncContext);

        if( M4NO_ERROR != err )
        {
            M4OSA_TRACE1_1(
                "M4VSS3GPP_intCreateVideoEncoder: pVideoEncoderGlobalFcts->pFctStart returns 0x%x",
                err);
            return err;
        }
    }

    pC->ewc.encoderState = M4VSS3GPP_kEncoderRunning;

    /**
    *    Return */
    M4OSA_TRACE3_0("M4VSS3GPP_intCreateVideoEncoder: returning M4NO_ERROR");
    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intDestroyVideoEncoder()
 * @brief    Destroy the video encoder
 * @note
 ******************************************************************************
 */
M4OSA_ERR M4VSS3GPP_intDestroyVideoEncoder( M4VSS3GPP_InternalEditContext *pC )
{
    M4OSA_ERR err = M4NO_ERROR;

    if( M4OSA_NULL != pC->ewc.pEncContext )
    {
        if( M4VSS3GPP_kEncoderRunning == pC->ewc.encoderState )
        {
            if( pC->ShellAPI.pVideoEncoderGlobalFcts->pFctStop != M4OSA_NULL )
            {
                err = pC->ShellAPI.pVideoEncoderGlobalFcts->pFctStop(
                    pC->ewc.pEncContext);

                if( M4NO_ERROR != err )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intDestroyVideoEncoder:\
                        pVideoEncoderGlobalFcts->pFctStop returns 0x%x",
                        err);
                    /* Well... how the heck do you handle a failed cleanup? */
                }
            }

            pC->ewc.encoderState = M4VSS3GPP_kEncoderStopped;
        }

        /* Has the encoder actually been opened? Don't close it if that's not the case. */
        if( M4VSS3GPP_kEncoderStopped == pC->ewc.encoderState )
        {
            err = pC->ShellAPI.pVideoEncoderGlobalFcts->pFctClose(
                pC->ewc.pEncContext);

            if( M4NO_ERROR != err )
            {
                M4OSA_TRACE1_1(
                    "M4VSS3GPP_intDestroyVideoEncoder:\
                    pVideoEncoderGlobalFcts->pFctClose returns 0x%x",
                    err);
                /* Well... how the heck do you handle a failed cleanup? */
            }

            pC->ewc.encoderState = M4VSS3GPP_kEncoderClosed;
        }

        err = pC->ShellAPI.pVideoEncoderGlobalFcts->pFctCleanup(
            pC->ewc.pEncContext);

        if( M4NO_ERROR != err )
        {
            M4OSA_TRACE1_1(
                "M4VSS3GPP_intDestroyVideoEncoder:\
                pVideoEncoderGlobalFcts->pFctCleanup returns 0x%x!",
                err);
            /**< We do not return the error here because we still have stuff to free */
        }

        pC->ewc.encoderState = M4VSS3GPP_kNoEncoder;
        /**
        * Reset variable */
        pC->ewc.pEncContext = M4OSA_NULL;
    }

    M4OSA_TRACE3_1("M4VSS3GPP_intDestroyVideoEncoder: returning 0x%x", err);
    return err;
}

/**
 ******************************************************************************
 * M4OSA_Void M4VSS3GPP_intSetH263TimeCounter()
 * @brief    Modify the time counter of the given H263 video AU
 * @note
 * @param    pAuDataBuffer    (IN/OUT) H263 Video AU to modify
 * @param    uiCts            (IN)     New time counter value
 * @return    nothing
 ******************************************************************************
 */
static M4OSA_Void M4VSS3GPP_intSetH263TimeCounter( M4OSA_MemAddr8 pAuDataBuffer,
                                                  M4OSA_UInt8 uiCts )
{
    /*
    *  The H263 time counter is 8 bits located on the "x" below:
    *
    *   |--------|--------|--------|--------|
    *    ???????? ???????? ??????xx xxxxxx??
    */

    /**
    * Write the 2 bits on the third byte */
    pAuDataBuffer[2] = ( pAuDataBuffer[2] & 0xFC) | (( uiCts >> 6) & 0x3);

    /**
    * Write the 6 bits on the fourth byte */
    pAuDataBuffer[3] = ( ( uiCts << 2) & 0xFC) | (pAuDataBuffer[3] & 0x3);

    return;
}

/**
 ******************************************************************************
 * M4OSA_Void M4VSS3GPP_intSetMPEG4Gov()
 * @brief    Modify the time info from Group Of VOP video AU
 * @note
 * @param    pAuDataBuffer    (IN)    MPEG4 Video AU to modify
 * @param    uiCtsSec            (IN)     New GOV time info in second unit
 * @return    nothing
 ******************************************************************************
 */
static M4OSA_Void M4VSS3GPP_intSetMPEG4Gov( M4OSA_MemAddr8 pAuDataBuffer,
                                           M4OSA_UInt32 uiCtsSec )
{
    /*
    *  The MPEG-4 time code length is 18 bits:
    *
    *     hh     mm    marker    ss
    *    xxxxx|xxx xxx     1    xxxx xx ??????
    *   |----- ---|---     -    ----|-- ------|
    */
    M4OSA_UInt8 uiHh;
    M4OSA_UInt8 uiMm;
    M4OSA_UInt8 uiSs;
    M4OSA_UInt8 uiTmp;

    /**
    * Write the 2 last bits ss */
    uiSs = (M4OSA_UInt8)(uiCtsSec % 60); /**< modulo part */
    pAuDataBuffer[2] = (( ( uiSs & 0x03) << 6) | (pAuDataBuffer[2] & 0x3F));

    if( uiCtsSec < 60 )
    {
        /**
        * Write the 3 last bits of mm, the marker bit (0x10 */
        pAuDataBuffer[1] = (( 0x10) | (uiSs >> 2));

        /**
        * Write the 5 bits of hh and 3 of mm (out of 6) */
        pAuDataBuffer[0] = 0;
    }
    else
    {
        /**
        * Write the 3 last bits of mm, the marker bit (0x10 */
        uiTmp = (M4OSA_UInt8)(uiCtsSec / 60); /**< integer part */
        uiMm = (M4OSA_UInt8)(uiTmp % 60);
        pAuDataBuffer[1] = (( uiMm << 5) | (0x10) | (uiSs >> 2));

        if( uiTmp < 60 )
        {
            /**
            * Write the 5 bits of hh and 3 of mm (out of 6) */
            pAuDataBuffer[0] = ((uiMm >> 3));
        }
        else
        {
            /**
            * Write the 5 bits of hh and 3 of mm (out of 6) */
            uiHh = (M4OSA_UInt8)(uiTmp / 60);
            pAuDataBuffer[0] = (( uiHh << 3) | (uiMm >> 3));
        }
    }
    return;
}

/**
 ******************************************************************************
 * M4OSA_Void M4VSS3GPP_intGetMPEG4Gov()
 * @brief    Get the time info from Group Of VOP video AU
 * @note
 * @param    pAuDataBuffer    (IN)    MPEG4 Video AU to modify
 * @param    pCtsSec            (OUT)    Current GOV time info in second unit
 * @return    nothing
 ******************************************************************************
 */
static M4OSA_Void M4VSS3GPP_intGetMPEG4Gov( M4OSA_MemAddr8 pAuDataBuffer,
                                           M4OSA_UInt32 *pCtsSec )
{
    /*
    *  The MPEG-4 time code length is 18 bits:
    *
    *     hh     mm    marker    ss
    *    xxxxx|xxx xxx     1    xxxx xx ??????
    *   |----- ---|---     -    ----|-- ------|
    */
    M4OSA_UInt8 uiHh;
    M4OSA_UInt8 uiMm;
    M4OSA_UInt8 uiSs;
    M4OSA_UInt8 uiTmp;
    M4OSA_UInt32 uiCtsSec;

    /**
    * Read ss */
    uiSs = (( pAuDataBuffer[2] & 0xC0) >> 6);
    uiTmp = (( pAuDataBuffer[1] & 0x0F) << 2);
    uiCtsSec = uiSs + uiTmp;

    /**
    * Read mm */
    uiMm = (( pAuDataBuffer[1] & 0xE0) >> 5);
    uiTmp = (( pAuDataBuffer[0] & 0x07) << 3);
    uiMm = uiMm + uiTmp;
    uiCtsSec = ( uiMm * 60) + uiCtsSec;

    /**
    * Read hh */
    uiHh = (( pAuDataBuffer[0] & 0xF8) >> 3);

    if( uiHh )
    {
        uiCtsSec = ( uiHh * 3600) + uiCtsSec;
    }

    /*
    * in sec */
    *pCtsSec = uiCtsSec;

    return;
}

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intAllocateYUV420()
 * @brief    Allocate the three YUV 4:2:0 planes
 * @note
 * @param    pPlanes    (IN/OUT) valid pointer to 3 M4VIFI_ImagePlane structures
 * @param    uiWidth    (IN)     Image width
 * @param    uiHeight(IN)     Image height
 ******************************************************************************
 */
static M4OSA_ERR M4VSS3GPP_intAllocateYUV420( M4VIFI_ImagePlane *pPlanes,
                                             M4OSA_UInt32 uiWidth, M4OSA_UInt32 uiHeight )
{

    pPlanes[0].u_width = uiWidth;
    pPlanes[0].u_height = uiHeight;
    pPlanes[0].u_stride = uiWidth;
    pPlanes[0].u_topleft = 0;
    pPlanes[0].pac_data = (M4VIFI_UInt8 *)M4OSA_32bitAlignedMalloc(pPlanes[0].u_stride
        * pPlanes[0].u_height, M4VSS3GPP, (M4OSA_Char *)"pPlanes[0].pac_data");

    if( M4OSA_NULL == pPlanes[0].pac_data )
    {
        M4OSA_TRACE1_0(
            "M4VSS3GPP_intAllocateYUV420: unable to allocate pPlanes[0].pac_data,\
            returning M4ERR_ALLOC");
        return M4ERR_ALLOC;
    }

    pPlanes[1].u_width = pPlanes[0].u_width >> 1;
    pPlanes[1].u_height = pPlanes[0].u_height >> 1;
    pPlanes[1].u_stride = pPlanes[1].u_width;
    pPlanes[1].u_topleft = 0;
    pPlanes[1].pac_data = (M4VIFI_UInt8 *)M4OSA_32bitAlignedMalloc(pPlanes[1].u_stride
        * pPlanes[1].u_height, M4VSS3GPP,(M4OSA_Char *) "pPlanes[1].pac_data");

    if( M4OSA_NULL == pPlanes[1].pac_data )
    {
        M4OSA_TRACE1_0(
            "M4VSS3GPP_intAllocateYUV420: unable to allocate pPlanes[1].pac_data,\
            returning M4ERR_ALLOC");
        return M4ERR_ALLOC;
    }

    pPlanes[2].u_width = pPlanes[1].u_width;
    pPlanes[2].u_height = pPlanes[1].u_height;
    pPlanes[2].u_stride = pPlanes[2].u_width;
    pPlanes[2].u_topleft = 0;
    pPlanes[2].pac_data = (M4VIFI_UInt8 *)M4OSA_32bitAlignedMalloc(pPlanes[2].u_stride
        * pPlanes[2].u_height, M4VSS3GPP, (M4OSA_Char *)"pPlanes[2].pac_data");

    if( M4OSA_NULL == pPlanes[2].pac_data )
    {
        M4OSA_TRACE1_0(
            "M4VSS3GPP_intAllocateYUV420: unable to allocate pPlanes[2].pac_data,\
            returning M4ERR_ALLOC");
        return M4ERR_ALLOC;
    }

    /**
    *    Return */
    M4OSA_TRACE3_0("M4VSS3GPP_intAllocateYUV420: returning M4NO_ERROR");
    return M4NO_ERROR;
}
