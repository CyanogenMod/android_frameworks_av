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
 * @file    M4VSS3GPP_ClipAnalysis.c
 * @brief    Implementation of functions related to analysis of input clips
 * @note    All functions in this file are static, i.e. non public
 ******************************************************************************
 */

/****************/
/*** Includes ***/
/****************/

#include "NXPSW_CompilerSwitches.h"
/**
 *    Our headers */
#include "M4VSS3GPP_API.h"
#include "M4VSS3GPP_ErrorCodes.h"
#include "M4VSS3GPP_InternalTypes.h"
#include "M4VSS3GPP_InternalFunctions.h"
#include "M4VSS3GPP_InternalConfig.h"


#ifdef M4VSS_ENABLE_EXTERNAL_DECODERS
#include "M4VD_EXTERNAL_Interface.h"

#endif

/**
 *    OSAL headers */
#include "M4OSA_Memory.h" /* OSAL memory management */
#include "M4OSA_Debug.h"  /* OSAL debug management */

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_editAnalyseClip()
 * @brief    This function allows checking if a clip is compatible with VSS 3GPP editing
 * @note    It also fills a ClipAnalysis structure, which can be used to check if two
 *        clips are compatible
 * @param    pClip                (IN) File descriptor of the input 3GPP/MP3 clip file.
 * @param    pClipProperties        (IN) Pointer to a valid ClipProperties structure.
 * @param    FileType            (IN) Type of the input file (.3gp, .amr, .mp3)
 * @return    M4NO_ERROR:            No error
 * @return    M4ERR_PARAMETER:    At least one parameter is M4OSA_NULL (debug only)
 * @return   M4VSS3GPP_ERR_H263_PROFILE_NOT_SUPPORTED
 * @return   M4VSS3GPP_ERR_INVALID_CLIP_ANALYSIS_VERSION
 * @return   M4VSS3GPP_ERR_AMR_EDITING_UNSUPPORTED
 * @return   M4VSS3GPP_ERR_EDITING_UNSUPPORTED_H263_PROFILE
 * @return   M4VSS3GPP_ERR_EDITING_UNSUPPORTED_MPEG4_PROFILE
 * @return   M4VSS3GPP_ERR_EDITING_UNSUPPORTED_MPEG4_RVLC
 * @return   M4VSS3GPP_ERR_UNSUPPORTED_INPUT_VIDEO_FORMAT
 * @return   M4VSS3GPP_ERR_EDITING_NO_SUPPORTED_VIDEO_STREAM_IN_FILE
 * @return   M4VSS3GPP_ERR_EDITING_UNSUPPORTED_AUDIO_FORMAT
 * @return   M4VSS3GPP_ERR_EDITING_NO_SUPPORTED_STREAM_IN_FILE
 ******************************************************************************
 */
M4OSA_ERR M4VSS3GPP_editAnalyseClip( M4OSA_Void *pClip,
                                    M4VIDEOEDITING_FileType FileType,
                                    M4VIDEOEDITING_ClipProperties *pClipProperties,
                                    M4OSA_FileReadPointer *pFileReadPtrFct )
{
    M4OSA_ERR err;
    M4VSS3GPP_ClipContext *pClipContext;
    M4VSS3GPP_ClipSettings ClipSettings;

    M4OSA_TRACE3_2(
        "M4VSS3GPP_editAnalyseClip called with pClip=0x%x, pClipProperties=0x%x",
        pClip, pClipProperties);

    /**
    *    Check input parameter */
    M4OSA_DEBUG_IF2((M4OSA_NULL == pClip), M4ERR_PARAMETER,
        "M4VSS3GPP_editAnalyseClip: pClip is M4OSA_NULL");
    M4OSA_DEBUG_IF2((M4OSA_NULL == pClipProperties), M4ERR_PARAMETER,
        "M4VSS3GPP_editAnalyseClip: pClipProperties is M4OSA_NULL");

    /**
    * Build dummy clip settings, in order to use the editClipOpen function */
    ClipSettings.pFile = pClip;
    ClipSettings.FileType = FileType;
    ClipSettings.uiBeginCutTime = 0;
    ClipSettings.uiEndCutTime = 0;

    /* Clip properties not build yet, set at least this flag */
    ClipSettings.ClipProperties.bAnalysed = M4OSA_FALSE;

    /**
    * Open the clip in fast open mode */
    err = M4VSS3GPP_intClipInit(&pClipContext, pFileReadPtrFct);

    if( M4NO_ERROR != err )
    {
        M4OSA_TRACE1_1(
            "M4VSS3GPP_editAnalyseClip: M4VSS3GPP_intClipInit() returns 0x%x!",
            err);

        /**
        * Free the clip */
        if( M4OSA_NULL != pClipContext )
        {
            M4VSS3GPP_intClipCleanUp(pClipContext);
        }
        return err;
    }

    err = M4VSS3GPP_intClipOpen(pClipContext, &ClipSettings, M4OSA_FALSE,
        M4OSA_TRUE, M4OSA_TRUE);

    if( M4NO_ERROR != err )
    {
        M4OSA_TRACE1_1(
            "M4VSS3GPP_editAnalyseClip: M4VSS3GPP_intClipOpen() returns 0x%x!",
            err);

        M4VSS3GPP_intClipCleanUp(pClipContext);

        /**
        * Here it is better to return the Editing specific error code */
        if( ( ((M4OSA_UInt32)M4ERR_DECODER_H263_PROFILE_NOT_SUPPORTED) == err)
            || (((M4OSA_UInt32)M4ERR_DECODER_H263_NOT_BASELINE) == err) )
        {
            M4OSA_TRACE1_0(
                "M4VSS3GPP_editAnalyseClip:\
                M4VSS3GPP_intClipOpen() returns M4VSS3GPP_ERR_H263_PROFILE_NOT_SUPPORTED");
            return M4VSS3GPP_ERR_H263_PROFILE_NOT_SUPPORTED;
        }
        return err;
    }

    /**
    * Analyse the clip */
    err = M4VSS3GPP_intBuildAnalysis(pClipContext, pClipProperties);

    if( M4NO_ERROR != err )
    {
        M4OSA_TRACE1_1(
            "M4VSS3GPP_editAnalyseClip: M4VSS3GPP_intBuildAnalysis() returns 0x%x!",
            err);

        /**
        * Free the clip */
        M4VSS3GPP_intClipCleanUp(pClipContext);
        return err;
    }

    /**
    * Free the clip */
    err = M4VSS3GPP_intClipClose(pClipContext);

    if( M4NO_ERROR != err )
    {
        M4OSA_TRACE1_1(
            "M4VSS3GPP_editAnalyseClip: M4VSS_intClipClose() returns 0x%x!",
            err);
        M4VSS3GPP_intClipCleanUp(pClipContext);
        return err;
    }

    M4VSS3GPP_intClipCleanUp(pClipContext);

    /**
    * Check the clip is compatible with VSS editing */
    err = M4VSS3GPP_intCheckClipCompatibleWithVssEditing(pClipProperties);

    if( M4NO_ERROR != err )
    {
        M4OSA_TRACE1_1(
            "M4VSS3GPP_editAnalyseClip:\
            M4VSS3GPP_intCheckClipCompatibleWithVssEditing() returns 0x%x!",
            err);
        return err;
    }

    /**
    * Return with no error */
    M4OSA_TRACE3_0("M4VSS3GPP_editAnalyseClip(): returning M4NO_ERROR");
    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_editCheckClipCompatibility()
 * @brief    This function allows checking if two clips are compatible with each other for
 *        VSS 3GPP editing assembly feature.
 * @note
 * @param    pClip1Properties        (IN) Clip analysis of the first clip
 * @param    pClip2Properties        (IN) Clip analysis of the second clip
 * @return    M4NO_ERROR:            No error
 * @return    M4ERR_PARAMETER:    At least one parameter is M4OSA_NULL (debug only)
 * @return    M4VSS3GPP_ERR_INVALID_CLIP_ANALYSIS_VERSION
 * @return    M4VSS3GPP_ERR_INCOMPATIBLE_VIDEO_FORMAT
 * @return    M4VSS3GPP_ERR_INCOMPATIBLE_VIDEO_FRAME_SIZE
 * @return    M4VSS3GPP_ERR_INCOMPATIBLE_VIDEO_TIME_SCALE
 * @return    M4VSS3GPP_ERR_INCOMPATIBLE_VIDEO_DATA_PARTITIONING
 * @return  M4VSS3GPP_ERR_UNSUPPORTED_MP3_ASSEMBLY
 * @return  M4VSS3GPP_ERR_UNSUPPORTED_INPUT_VIDEO_FORMAT
 ******************************************************************************
 */
M4OSA_ERR M4VSS3GPP_editCheckClipCompatibility( M4VIDEOEDITING_ClipProperties *pClip1Properties,
                                                M4VIDEOEDITING_ClipProperties *pClip2Properties )
{
    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_ERR video_err = M4NO_ERROR;
    M4OSA_ERR audio_err = M4NO_ERROR;

    M4OSA_Bool bClip1IsAAC = M4OSA_FALSE;
    M4OSA_Bool bClip2IsAAC = M4OSA_FALSE;

    M4OSA_TRACE3_2("M4VSS3GPP_editCheckClipCompatibility called with pClip1Analysis=0x%x,\
                   pClip2Analysis=0x%x", pClip1Properties, pClip2Properties);

    /**
    *    Check input parameter */
    M4OSA_DEBUG_IF2((M4OSA_NULL == pClip1Properties), M4ERR_PARAMETER,
        "M4VSS3GPP_editCheckClipCompatibility: pClip1Properties is M4OSA_NULL");
    M4OSA_DEBUG_IF2((M4OSA_NULL == pClip2Properties), M4ERR_PARAMETER,
        "M4VSS3GPP_editCheckClipCompatibility: pClip2Properties is M4OSA_NULL");

    /**
    * Check if the two clips are, alone, comptible with VSS 3GPP.
    *
    * Note: if a clip is not compatible with VSS3GPP, M4VSS3GPP_editAnalyseClip()
    * did return an error to the integrator. So he should not call
    * M4VSS3GPP_editCheckClipCompatibility
    * with the ClipAnalysis...
    * Still, I think it is good to redo the test here, to be sure.
    * M4VSS3GPP_intCheckClipCompatibleWithVssEditing is not a long function to execute.*/
    err = M4VSS3GPP_intCheckClipCompatibleWithVssEditing(pClip1Properties);

    if( err != M4NO_ERROR )
    {
        M4OSA_TRACE1_1(
            "M4VSS3GPP_editCheckClipCompatibility: Clip1 not compatible with VSS3GPP,\
            returning 0x%x", err);
        return err;
    }
    err = M4VSS3GPP_intCheckClipCompatibleWithVssEditing(pClip2Properties);

    if( err != M4NO_ERROR )
    {
        M4OSA_TRACE1_1(
            "M4VSS3GPP_editCheckClipCompatibility: Clip2 not compatible with VSS3GPP,\
            returning 0x%x", err);
        return err;
    }

    if( ( M4VIDEOEDITING_kFileType_MP3 == pClip1Properties->FileType)
        || (M4VIDEOEDITING_kFileType_AMR == pClip1Properties->FileType) )
    {
        if( pClip1Properties != pClip2Properties )
        {
            M4OSA_TRACE1_0(
                "M4VSS3GPP_editCheckClipCompatibility: MP3 CAN ONLY BE CUT,\
                returning M4VSS3GPP_ERR_UNSUPPORTED_MP3_ASSEMBLY");
            return M4VSS3GPP_ERR_UNSUPPORTED_MP3_ASSEMBLY;
        }
        else
        {
            /* We are in VSS Splitter mode */
            goto audio_analysis;
        }
    }

    /********** Video ************/

    /**
    * Check both clips have same video stream type */
    if( pClip1Properties->VideoStreamType != pClip2Properties->VideoStreamType )
    {
        M4OSA_TRACE1_0(
            "M4VSS3GPP_editCheckClipCompatibility: Clips don't have the same video format");
        video_err = M4VSS3GPP_ERR_INCOMPATIBLE_VIDEO_FORMAT;
        goto audio_analysis;
    }

    /**
    * Check both clips have the same video frame size */
    if( ( pClip1Properties->uiVideoWidth != pClip2Properties->uiVideoWidth)
        || (pClip1Properties->uiVideoHeight
        != pClip2Properties->uiVideoHeight) )
    {
        M4OSA_TRACE1_0(
            "M4VSS3GPP_editCheckClipCompatibility: Clips don't have the same video frame size");
        video_err = M4VSS3GPP_ERR_INCOMPATIBLE_VIDEO_FRAME_SIZE;
        goto audio_analysis;
    }

    switch( pClip1Properties->VideoStreamType )
    {
        case M4VIDEOEDITING_kH263:
        case M4VIDEOEDITING_kH264:
            /**< nothing to check here */
            break;

        case M4VIDEOEDITING_kMPEG4_EMP:
        case M4VIDEOEDITING_kMPEG4:
            /**
            * Check both streams have the same time scale */
            if( pClip1Properties->uiVideoTimeScale
                != pClip2Properties->uiVideoTimeScale )
            {
                M4OSA_TRACE1_2(
                    "M4VSS3GPP_editCheckClipCompatibility: Clips don't have the same video time\
                    scale (%d != %d), returning M4VSS3GPP_ERR_INCOMPATIBLE_VIDEO_TIME_SCALE",
                    pClip1Properties->uiVideoTimeScale,
                    pClip2Properties->uiVideoTimeScale);
                video_err = M4VSS3GPP_ERR_INCOMPATIBLE_VIDEO_TIME_SCALE;
                goto audio_analysis;
            }
            /**
            * Check both streams have the same use of data partitioning */
            if( pClip1Properties->bMPEG4dataPartition
                != pClip2Properties->bMPEG4dataPartition )
            {
                M4OSA_TRACE1_2(
                    "M4VSS3GPP_editCheckClipCompatibility:\
                    Clips don't have the same use of data partitioning (%d != %d),\
                    returning M4VSS3GPP_ERR_INCOMPATIBLE_VIDEO_DATA_PARTITIONING",
                    pClip1Properties->bMPEG4dataPartition,
                    pClip2Properties->bMPEG4dataPartition);
                video_err = M4VSS3GPP_ERR_INCOMPATIBLE_VIDEO_DATA_PARTITIONING;
                goto audio_analysis;
            }
            break;

        default:
            M4OSA_TRACE1_1(
                "M4VSS3GPP_editCheckClipCompatibility: unknown video stream type (0x%x),\
                returning M4VSS3GPP_ERR_UNSUPPORTED_INPUT_VIDEO_FORMAT",
                pClip1Properties->VideoStreamType);
            video_err =
                M4VSS3GPP_ERR_UNSUPPORTED_INPUT_VIDEO_FORMAT; /**< this error should never happen,
                                                              it's here for code safety only... */
            goto audio_analysis;
    }

    pClip2Properties->bVideoIsCompatibleWithMasterClip = M4OSA_TRUE;

    /********** Audio ************/

audio_analysis:
    if( M4VIDEOEDITING_kNoneAudio != pClip1Properties->
        AudioStreamType ) /**< if there is an audio stream */
    {
        /**
        * Check audio format is AAC */
        switch( pClip1Properties->AudioStreamType )
        {
            case M4VIDEOEDITING_kAAC:
            case M4VIDEOEDITING_kAACplus:
            case M4VIDEOEDITING_keAACplus:
                bClip1IsAAC = M4OSA_TRUE;
                break;
            default:
                break;
        }
    }

    if( M4VIDEOEDITING_kNoneAudio != pClip2Properties->
        AudioStreamType ) /**< if there is an audio stream */
    {
        /**
        * Check audio format is AAC */
        switch( pClip2Properties->AudioStreamType )
        {
            case M4VIDEOEDITING_kAAC:
            case M4VIDEOEDITING_kAACplus:
            case M4VIDEOEDITING_keAACplus:
                bClip2IsAAC = M4OSA_TRUE;
                break;
            default:
                break;
        }
    }

    /**
    * If there is no audio, the clips are compatibles ... */
    if( ( pClip1Properties->AudioStreamType != M4VIDEOEDITING_kNoneAudio)
        && (pClip2Properties->AudioStreamType != M4VIDEOEDITING_kNoneAudio) )
    {
        /**
        * Check both clips have same audio stream type
        * And let_s say AAC, AAC+ and eAAC+ are mixable */
        if( ( pClip1Properties->AudioStreamType
            != pClip2Properties->AudioStreamType)
            && (( M4OSA_FALSE == bClip1IsAAC) || (M4OSA_FALSE == bClip2IsAAC)) )
        {
            M4OSA_TRACE1_0(
                "M4VSS3GPP_editCheckClipCompatibility:\
                Clips don't have the same Audio Stream Type");

            audio_err = M4VSS3GPP_WAR_INCOMPATIBLE_AUDIO_STREAM_TYPE;
            goto analysis_done;
        }

        /**
        * Check both clips have same number of channels */
        if( pClip1Properties->uiNbChannels != pClip2Properties->uiNbChannels )
        {
            M4OSA_TRACE1_0(
                "M4VSS3GPP_editCheckClipCompatibility: Clips don't have the same Nb of Channels");
            audio_err = M4VSS3GPP_WAR_INCOMPATIBLE_AUDIO_NB_OF_CHANNELS;
            goto analysis_done;
        }

        /**
        * Check both clips have same sampling frequency */
        if( pClip1Properties->uiSamplingFrequency
            != pClip2Properties->uiSamplingFrequency )
        {
            M4OSA_TRACE1_0(
                "M4VSS3GPP_editCheckClipCompatibility:\
                Clips don't have the same Sampling Frequency");
            audio_err = M4VSS3GPP_WAR_INCOMPATIBLE_AUDIO_SAMPLING_FREQUENCY;
            goto analysis_done;
        }
    }

    pClip2Properties->bAudioIsCompatibleWithMasterClip = M4OSA_TRUE;

    /**
    * Return with no error */

analysis_done:
    if( video_err != M4NO_ERROR )
        return video_err;

    if( audio_err != M4NO_ERROR )
        return audio_err;

    M4OSA_TRACE3_0(
        "M4VSS3GPP_editCheckClipCompatibility(): returning M4NO_ERROR");
    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intBuildAnalysis()
 * @brief    Get video and audio properties from the clip streams
 * @note    This function must return fatal errors only (errors that should not happen
 *        in the final integrated product).
 * @param   pClipCtxt            (IN) internal clip context
 * @param    pClipProperties        (OUT) Pointer to a valid ClipProperties structure.
 * @return    M4NO_ERROR:            No error
 ******************************************************************************
 */
M4OSA_ERR M4VSS3GPP_intBuildAnalysis( M4VSS3GPP_ClipContext *pClipCtxt,
                                     M4VIDEOEDITING_ClipProperties *pClipProperties )
{
    M4OSA_ERR err;
    M4DECODER_MPEG4_DecoderConfigInfo DecConfigInfo;
    M4DECODER_VideoSize dummySize;
    M4DECODER_AVCProfileLevel AVCProfle;

    pClipProperties->bAnalysed = M4OSA_FALSE;

    /**
    * Reset video characteristics */
    pClipProperties->VideoStreamType = M4VIDEOEDITING_kNoneVideo;
    pClipProperties->uiClipVideoDuration = 0;
    pClipProperties->uiVideoBitrate = 0;
    pClipProperties->uiVideoMaxAuSize = 0;
    pClipProperties->uiVideoWidth = 0;
    pClipProperties->uiVideoHeight = 0;
    pClipProperties->uiVideoTimeScale = 0;
    pClipProperties->fAverageFrameRate = 0.0;
    pClipProperties->ProfileAndLevel =
        M4VIDEOEDITING_kProfile_and_Level_Out_Of_Range;
    pClipProperties->uiH263level = 0;
    pClipProperties->uiVideoProfile = 0;
    pClipProperties->bMPEG4dataPartition = M4OSA_FALSE;
    pClipProperties->bMPEG4rvlc = M4OSA_FALSE;
    pClipProperties->bMPEG4resynchMarker = M4OSA_FALSE;

    memset((void *) &pClipProperties->ftyp,0,
        sizeof(pClipProperties->ftyp));

    /**
    * Video Analysis */
    if( M4OSA_NULL != pClipCtxt->pVideoStream )
    {
        pClipProperties->uiVideoWidth = pClipCtxt->pVideoStream->m_videoWidth;
        pClipProperties->uiVideoHeight = pClipCtxt->pVideoStream->m_videoHeight;
        pClipProperties->fAverageFrameRate =
            pClipCtxt->pVideoStream->m_averageFrameRate;

        switch( pClipCtxt->pVideoStream->m_basicProperties.m_streamType )
        {
            case M4DA_StreamTypeVideoMpeg4:

                pClipProperties->VideoStreamType = M4VIDEOEDITING_kMPEG4;

#ifdef M4VSS_ENABLE_EXTERNAL_DECODERS
   /* This issue is so incredibly stupid that it's depressing. Basically, a file can be analysed
   outside of any context (besides that of the clip itself), so that for instance two clips can
   be checked for compatibility before allocating an edit context for editing them. But this
   means there is no way in heck to pass an external video decoder (to begin with) to this
   function, as they work by being registered in an existing context; furthermore, it is actually
   pretty overkill to use a full decoder for that, moreso a HARDWARE decoder just to get the
   clip config info. In fact, the hardware itself doesn't provide this service, in the case of a
   HW decoder, the shell builds the config info itself, so we don't need the actual decoder, only
   a detached functionality of it. So in case HW/external decoders may be present, we instead use
   directly the DSI parsing function of the shell HW decoder (which we know to be present, since
   HW decoders are possible) to get the config info. Notice this function is used even if the
   software decoder is actually present and even if it will end up being actually used: figuring
   out the config does not involve actual decoding nor the particularities of a specific decoder,
   it's the fact that it's MPEG4 that matters, so it should not be functionally any different
   from the way it was done before (and it's light enough for performance not to be any problem
         whatsoever). */

                err = M4DECODER_EXTERNAL_ParseVideoDSI(pClipCtxt->pVideoStream->
                    m_basicProperties.m_pDecoderSpecificInfo,
                    pClipCtxt->pVideoStream->
                    m_basicProperties.m_decoderSpecificInfoSize,
                    &DecConfigInfo, &dummySize);

                if( M4NO_ERROR != err )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intBuildAnalysis():\
                        M4DECODER_EXTERNAL_ParseVideoDSI returns 0x%08X", err);
                    return err;
                }

    #else /* an external decoder cannot be present, so we can rely on the
                software decoder to be installed already */
                /* Get MPEG-4 decoder config. */

                err = pClipCtxt->ShellAPI.m_pVideoDecoder->m_pFctGetOption(
                    pClipCtxt->pViDecCtxt,
                    M4DECODER_MPEG4_kOptionID_DecoderConfigInfo,
                    &DecConfigInfo);

                if( M4NO_ERROR != err )
                {
                    M4OSA_TRACE1_1("M4VSS3GPP_intBuildAnalysis(): m_pFctGetOption(DecConfigInfo)\
                        returns 0x%x", err);
                    return err;
                }

    #endif /* M4VSS_ENABLE_EXTERNAL_DECODERS */

                pClipProperties->uiVideoProfile = DecConfigInfo.uiProfile;
                pClipProperties->uiVideoTimeScale = DecConfigInfo.uiTimeScale;
                pClipProperties->bMPEG4dataPartition =
                    DecConfigInfo.bDataPartition;
                pClipProperties->bMPEG4rvlc = DecConfigInfo.bUseOfRVLC;
                pClipProperties->bMPEG4resynchMarker =
                    DecConfigInfo.uiUseOfResynchMarker;

                /* Supported enum value for profile and level */
                switch( pClipProperties->uiVideoProfile )
                {
                    case 0x08:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kMPEG4_SP_Level_0;
                        break;

                    case 0x09:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kMPEG4_SP_Level_0b;
                        break;

                    case 0x01:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kMPEG4_SP_Level_1;
                        break;

                    case 0x02:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kMPEG4_SP_Level_2;
                        break;

                    case 0x03:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kMPEG4_SP_Level_3;
                        break;

                    case 0x04:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kMPEG4_SP_Level_4a;
                        break;

                    case 0x05:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kMPEG4_SP_Level_5;
                        break;
                }
                break;

            case M4DA_StreamTypeVideoH263:

                pClipProperties->VideoStreamType = M4VIDEOEDITING_kH263;

                /* Get H263 level, which is sixth byte in the DSI */
                pClipProperties->uiH263level = pClipCtxt->pVideoStream->
                    m_basicProperties.m_pDecoderSpecificInfo[5];
                /* Get H263 profile, which is fifth byte in the DSI */
                pClipProperties->uiVideoProfile = pClipCtxt->pVideoStream->
                    m_basicProperties.m_pDecoderSpecificInfo[6];
                /* H263 time scale is always 30000 */
                pClipProperties->uiVideoTimeScale = 30000;

                /* Supported enum value for profile and level */
                if( pClipProperties->uiVideoProfile == 0 )
                {
                    switch( pClipProperties->uiH263level )
                    {
                        case 10:
                            pClipProperties->ProfileAndLevel =
                                M4VIDEOEDITING_kH263_Profile_0_Level_10;
                            break;

                        case 20:
                            pClipProperties->ProfileAndLevel =
                                M4VIDEOEDITING_kH263_Profile_0_Level_20;
                            break;

                        case 30:
                            pClipProperties->ProfileAndLevel =
                                M4VIDEOEDITING_kH263_Profile_0_Level_30;
                            break;

                        case 40:
                            pClipProperties->ProfileAndLevel =
                                M4VIDEOEDITING_kH263_Profile_0_Level_40;
                            break;

                        case 45:
                            pClipProperties->ProfileAndLevel =
                                M4VIDEOEDITING_kH263_Profile_0_Level_45;
                            break;
                    }
                }
                break;

            case M4DA_StreamTypeVideoMpeg4Avc:

                pClipProperties->VideoStreamType = M4VIDEOEDITING_kH264;
#ifdef M4VSS_ENABLE_EXTERNAL_DECODERS

                err = M4DECODER_EXTERNAL_ParseAVCDSI(pClipCtxt->pVideoStream->
                    m_basicProperties.m_pDecoderSpecificInfo,
                    pClipCtxt->pVideoStream->
                    m_basicProperties.m_decoderSpecificInfoSize,
                    &AVCProfle);

                if( M4NO_ERROR != err )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intBuildAnalysis(): \
                         M4DECODER_EXTERNAL_ParseAVCDSI returns 0x%08X",
                         err);
                    return err;
                }

#else /* an external decoder cannot be present, so we can rely on the
                software decoder to be installed already */

                err = pClipCtxt->ShellAPI.m_pVideoDecoder->m_pFctGetOption(
                    pClipCtxt->pViDecCtxt,
                    M4DECODER_kOptionID_AVCProfileAndLevel, &AVCProfle);

                if( M4NO_ERROR != err )
                {
                    M4OSA_TRACE1_1(
                        "M4VSS3GPP_intBuildAnalysis(): m_pFctGetOption(AVCProfileInfo)\
                            returns 0x%x", err);
                    return err;
                }

#endif /* M4VSS_ENABLE_EXTERNAL_DECODERS */

                switch( AVCProfle )
                {
                    case M4DECODER_AVC_kProfile_0_Level_1:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_1;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_1b:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_1b;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_1_1:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_1_1;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_1_2:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_1_2;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_1_3:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_1_3;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_2:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_2;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_2_1:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_2_1;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_2_2:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_2_2;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_3:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_3;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_3_1:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_3_1;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_3_2:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_3_2;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_4:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_4;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_4_1:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_4_1;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_4_2:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_4_2;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_5:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_5;
                        break;

                    case M4DECODER_AVC_kProfile_0_Level_5_1:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kH264_Profile_0_Level_5_1;
                        break;

                    case M4DECODER_AVC_kProfile_and_Level_Out_Of_Range:
                    default:
                        pClipProperties->ProfileAndLevel =
                            M4VIDEOEDITING_kProfile_and_Level_Out_Of_Range;
                }

                break;

            default:
                M4OSA_TRACE1_1(
                    "M4VSS3GPP_intBuildAnalysis: unknown input video format (0x%x),\
                    returning M4NO_ERROR",pClipCtxt->pVideoStream->m_basicProperties.m_streamType);
                return
                    M4NO_ERROR; /**< We do not return error here.
                                The video format compatibility check will be done latter */
        }

        pClipProperties->uiClipVideoDuration =
            (M4OSA_UInt32)pClipCtxt->pVideoStream->m_basicProperties.m_duration;
        pClipProperties->uiVideoMaxAuSize =
            pClipCtxt->pVideoStream->m_basicProperties.m_maxAUSize;

        /* if video bitrate not available retrieve an estimation of the overall bitrate */
        pClipProperties->uiVideoBitrate =
            (M4OSA_UInt32)pClipCtxt->pVideoStream->
            m_basicProperties.m_averageBitRate;

        if( 0 == pClipProperties->uiVideoBitrate )
        {
            pClipCtxt->ShellAPI.m_pReader->m_pFctGetOption(
                pClipCtxt->pReaderContext, M4READER_kOptionID_Bitrate,
                &pClipProperties->uiVideoBitrate);

            if( M4OSA_NULL != pClipCtxt->pAudioStream )
            {
                /* we get the overall bitrate, substract the audio bitrate if any */
                pClipProperties->uiVideoBitrate -=
                    pClipCtxt->pAudioStream->m_basicProperties.m_averageBitRate;
            }
        }
    }

    /**
    * Reset audio characteristics */
    pClipProperties->AudioStreamType = M4VIDEOEDITING_kNoneAudio;
    pClipProperties->uiClipAudioDuration = 0;
    pClipProperties->uiAudioBitrate = 0;
    pClipProperties->uiAudioMaxAuSize = 0;
    pClipProperties->uiNbChannels = 0;
    pClipProperties->uiSamplingFrequency = 0;
    pClipProperties->uiExtendedSamplingFrequency = 0;
    pClipProperties->uiDecodedPcmSize = 0;

    /**
    * Audio Analysis */
    if( M4OSA_NULL != pClipCtxt->pAudioStream )
    {
        switch( pClipCtxt->pAudioStream->m_basicProperties.m_streamType )
        {
            case M4DA_StreamTypeAudioAmrNarrowBand:

                pClipProperties->AudioStreamType = M4VIDEOEDITING_kAMR_NB;
                break;

            case M4DA_StreamTypeAudioAac:

                pClipProperties->AudioStreamType = M4VIDEOEDITING_kAAC;
                break;

            case M4DA_StreamTypeAudioMp3:

                pClipProperties->AudioStreamType = M4VIDEOEDITING_kMP3;
                break;

            case M4DA_StreamTypeAudioEvrc:

                pClipProperties->AudioStreamType = M4VIDEOEDITING_kEVRC;
                break;

            case M4DA_StreamTypeAudioPcm:

                pClipProperties->AudioStreamType = M4VIDEOEDITING_kPCM;
                break;

            default:

                M4OSA_TRACE1_1(
                    "M4VSS3GPP_intBuildAnalysis: unknown input audio format (0x%x),\
                    returning M4NO_ERROR!",
                    pClipCtxt->pAudioStream->m_basicProperties.m_streamType);
                return
                    M4NO_ERROR; /**< We do not return error here.
                                The audio format compatibility check will be done latter */
        }

        pClipProperties->uiAudioMaxAuSize =
            pClipCtxt->pAudioStream->m_basicProperties.m_maxAUSize;
        pClipProperties->uiClipAudioDuration =
            (M4OSA_UInt32)pClipCtxt->pAudioStream->m_basicProperties.m_duration;

        pClipProperties->uiNbChannels = pClipCtxt->pAudioStream->m_nbChannels;
        pClipProperties->uiSamplingFrequency =
            pClipCtxt->pAudioStream->m_samplingFrequency;
        pClipProperties->uiDecodedPcmSize =
            pClipCtxt->pAudioStream->m_byteFrameLength
            * pClipCtxt->pAudioStream->m_byteSampleSize
            * pClipCtxt->pAudioStream->m_nbChannels;

        /**
        * Bugfix P4ME00001128: With some IMTC files, the AMR bit rate is 0 kbps
        according the GetProperties function */
        pClipProperties->uiAudioBitrate =
            (M4OSA_UInt32)pClipCtxt->pAudioStream->
            m_basicProperties.m_averageBitRate;

        if( 0 == pClipProperties->uiAudioBitrate )
        {
            if( M4VIDEOEDITING_kAMR_NB == pClipProperties->AudioStreamType )
            {
                /**
                *Better returning a guessed 12.2 kbps value than a sure-to-be-false 0 kbps value!*/
                pClipProperties->uiAudioBitrate = M4VSS3GPP_AMR_DEFAULT_BITRATE;
            }
            else if( M4VIDEOEDITING_kEVRC == pClipProperties->AudioStreamType )
            {
                /**
                *Better returning a guessed 9.2 kbps value than a sure-to-be-false 0 kbps value!*/
                pClipProperties->uiAudioBitrate =
                    M4VSS3GPP_EVRC_DEFAULT_BITRATE;
            }
            else
            {
                pClipCtxt->ShellAPI.m_pReader->m_pFctGetOption(
                    pClipCtxt->pReaderContext, M4READER_kOptionID_Bitrate,
                    &pClipProperties->uiAudioBitrate);

                if( M4OSA_NULL != pClipCtxt->pVideoStream )
                {
                    /* we get the overall bitrate, substract the video bitrate if any */
                    pClipProperties->uiAudioBitrate -= pClipCtxt->pVideoStream->
                        m_basicProperties.m_averageBitRate;
                }
            }
        }

        /* New aac properties */
        if( M4DA_StreamTypeAudioAac
            == pClipCtxt->pAudioStream->m_basicProperties.m_streamType )
        {
            pClipProperties->uiNbChannels = pClipCtxt->AacProperties.aNumChan;
            pClipProperties->uiSamplingFrequency =
                pClipCtxt->AacProperties.aSampFreq;

            if( pClipCtxt->AacProperties.aSBRPresent )
            {
                pClipProperties->AudioStreamType = M4VIDEOEDITING_kAACplus;
                pClipProperties->uiExtendedSamplingFrequency =
                    pClipCtxt->AacProperties.aExtensionSampFreq;
            }

            if( pClipCtxt->AacProperties.aPSPresent )
            {
                pClipProperties->AudioStreamType = M4VIDEOEDITING_keAACplus;
            }
        }
    }

    /* Get 'ftyp' atom */
    err = pClipCtxt->ShellAPI.m_pReader->m_pFctGetOption(
        pClipCtxt->pReaderContext,
        M4READER_kOptionID_3gpFtypBox, &pClipProperties->ftyp);

    if( M4NO_ERROR == err )
    {
        M4OSA_UInt8 i;

        for ( i = 0; i < pClipProperties->ftyp.nbCompatibleBrands; i++ )
            if( M4VIDEOEDITING_BRAND_EMP
                == pClipProperties->ftyp.compatible_brands[i] )
                pClipProperties->VideoStreamType = M4VIDEOEDITING_kMPEG4_EMP;
    }

    /**
    * We write the VSS 3GPP version in the clip analysis to be sure the integrator doesn't
    * mix older analysis results with newer libraries */
    pClipProperties->Version[0] = M4VIDEOEDITING_VERSION_MAJOR;
    pClipProperties->Version[1] = M4VIDEOEDITING_VERSION_MINOR;
    pClipProperties->Version[2] = M4VIDEOEDITING_VERSION_REVISION;

    pClipProperties->FileType = pClipCtxt->pSettings->FileType;

    if( pClipProperties->uiClipVideoDuration
        > pClipProperties->uiClipAudioDuration )
        pClipProperties->uiClipDuration = pClipProperties->uiClipVideoDuration;
    else
        pClipProperties->uiClipDuration = pClipProperties->uiClipAudioDuration;

    /* Reset compatibility chart */
    pClipProperties->bVideoIsEditable = M4OSA_FALSE;
    pClipProperties->bAudioIsEditable = M4OSA_FALSE;
    pClipProperties->bVideoIsCompatibleWithMasterClip = M4OSA_FALSE;
    pClipProperties->bAudioIsCompatibleWithMasterClip = M4OSA_FALSE;

    /* Analysis successfully completed */
    pClipProperties->bAnalysed = M4OSA_TRUE;

    /**
    * Return with no error */
    M4OSA_TRACE3_0("M4VSS3GPP_intBuildAnalysis(): returning M4NO_ERROR");
    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intCheckClipCompatibleWithVssEditing()
 * @brief    Check if the clip is compatible with VSS editing
 * @note
 * @param   pClipCtxt            (IN) internal clip context
 * @param    pClipProperties     (OUT) Pointer to a valid ClipProperties structure.
 * @return    M4NO_ERROR:            No error
 ******************************************************************************
 */
M4OSA_ERR M4VSS3GPP_intCheckClipCompatibleWithVssEditing(
    M4VIDEOEDITING_ClipProperties *pClipProperties )
{
    M4OSA_UInt32 uiNbOfValidStreams = 0;
    M4OSA_ERR video_err = M4NO_ERROR;
    M4OSA_ERR audio_err = M4NO_ERROR;

    /**
    * Check that analysis has been generated by this version of the VSS3GPP library */
    if( ( pClipProperties->Version[0] != M4VIDEOEDITING_VERSION_MAJOR)
        || (pClipProperties->Version[1] != M4VIDEOEDITING_VERSION_MINOR)
        || (pClipProperties->Version[2]
    != M4VIDEOEDITING_VERSION_REVISION) )
    {
        M4OSA_TRACE1_0(
            "M4VSS3GPP_intCheckClipCompatibleWithVssEditing: The clip analysis has been generated\
            by another version, returning M4VSS3GPP_ERR_INVALID_CLIP_ANALYSIS_VERSION");
        return M4VSS3GPP_ERR_INVALID_CLIP_ANALYSIS_VERSION;
    }

    /********* file type *********/

    if( M4VIDEOEDITING_kFileType_AMR == pClipProperties->FileType )
    {
        M4OSA_TRACE1_0(
            "M4VSS3GPP_intCheckClipCompatibleWithVssEditing:\
            returning M4VSS3GPP_ERR_AMR_EDITING_UNSUPPORTED");
        return M4VSS3GPP_ERR_AMR_EDITING_UNSUPPORTED;
    }

    if( M4VIDEOEDITING_kFileType_MP3 == pClipProperties->FileType )
    {
        M4OSA_TRACE3_0(
            "M4VSS3GPP_intCheckClipCompatibleWithVssEditing(): returning M4NO_ERROR");
        return M4NO_ERROR;
    }

    /********* Video *********/

    if( M4VIDEOEDITING_kNoneVideo
        != pClipProperties->VideoStreamType ) /**< if there is a video stream */
    {
        /**
        * Check video format is MPEG-4 or H263 */
        switch( pClipProperties->VideoStreamType )
        {
            case M4VIDEOEDITING_kH263:
                if( M4VIDEOEDITING_kProfile_and_Level_Out_Of_Range
                    == pClipProperties->ProfileAndLevel )
                {
                    M4OSA_TRACE1_0(
                        "M4VSS3GPP_intCheckClipCompatibleWithVssEditing():\
                        unsupported H263 profile");
                    video_err = M4VSS3GPP_ERR_EDITING_UNSUPPORTED_H263_PROFILE;
                    break;
                }
                uiNbOfValidStreams++;
                pClipProperties->bVideoIsEditable = M4OSA_TRUE;
                break;

            case M4VIDEOEDITING_kMPEG4_EMP:
            case M4VIDEOEDITING_kMPEG4:
                if( M4VIDEOEDITING_kProfile_and_Level_Out_Of_Range
                    == pClipProperties->ProfileAndLevel )
                {
                    M4OSA_TRACE1_0(
                        "M4VSS3GPP_intCheckClipCompatibleWithVssEditing():\
                        unsupported MPEG-4 profile");
                    video_err = M4VSS3GPP_ERR_EDITING_UNSUPPORTED_MPEG4_PROFILE;
                    break;
                }

                if( M4OSA_TRUE == pClipProperties->bMPEG4rvlc )
                {
                    M4OSA_TRACE1_0(
                        "M4VSS3GPP_intCheckClipCompatibleWithVssEditing():\
                        unsupported MPEG-4 RVLC tool");
                    video_err = M4VSS3GPP_ERR_EDITING_UNSUPPORTED_MPEG4_RVLC;
                    break;
                }
                uiNbOfValidStreams++;
                pClipProperties->bVideoIsEditable = M4OSA_TRUE;
                break;

            case M4VIDEOEDITING_kH264:
                if( M4VIDEOEDITING_kProfile_and_Level_Out_Of_Range
                    == pClipProperties->ProfileAndLevel )
                {
                    M4OSA_TRACE1_0(
                        "M4VSS3GPP_intCheckClipCompatibleWithVssEditing():\
                        unsupported H264 profile");
                    video_err = M4VSS3GPP_ERR_EDITING_UNSUPPORTED_H264_PROFILE;
                    break;
                }

                uiNbOfValidStreams++;
                pClipProperties->bVideoIsEditable = M4OSA_TRUE;
                break;

            default: /*< KO, we return error */
                M4OSA_TRACE1_0(
                    "M4VSS3GPP_intCheckClipCompatibleWithVssEditing(): unsupported video format");
                video_err = M4VSS3GPP_ERR_UNSUPPORTED_INPUT_VIDEO_FORMAT;
                break;
        }
    }
    else
    {
        /**
        * Audio only stream are currently not supported by the VSS editing feature
        (unless in the MP3 case) */
        M4OSA_TRACE1_0(
            "M4VSS3GPP_intCheckClipCompatibleWithVssEditing(): No video stream in clip");
        video_err = M4VSS3GPP_ERR_EDITING_NO_SUPPORTED_VIDEO_STREAM_IN_FILE;
    }

    /********* Audio *********/
    if( M4VIDEOEDITING_kNoneAudio != pClipProperties->
        AudioStreamType ) /**< if there is an audio stream */
    {
        /**
        * Check audio format is AMR-NB, EVRC or AAC */
        switch( pClipProperties->AudioStreamType )
        {
            case M4VIDEOEDITING_kAMR_NB:
                pClipProperties->bAudioIsEditable = M4OSA_TRUE;
                uiNbOfValidStreams++;
                break;

            case M4VIDEOEDITING_kAAC:
            case M4VIDEOEDITING_kAACplus:
            case M4VIDEOEDITING_keAACplus:
                switch( pClipProperties->uiSamplingFrequency )
                {
                case 8000:
                case 16000:
                case 22050:
                case 24000:
                case 32000:
                case 44100:
                case 48000:
                    pClipProperties->bAudioIsEditable = M4OSA_TRUE;
                    break;

                default:
                    break;
                }
                uiNbOfValidStreams++;
                break;

            case M4VIDEOEDITING_kEVRC:
                /*< OK, we proceed, no return */
                uiNbOfValidStreams++;
                break;

            default: /*< KO, we return error */
                M4OSA_TRACE1_0(
                    "M4VSS3GPP_intCheckClipCompatibleWithVssEditing(): unsupported audio format");
                audio_err = M4VSS3GPP_ERR_EDITING_UNSUPPORTED_AUDIO_FORMAT;
                break;
        }
    }
    else
    {
        /* Silence is always editable */
        pClipProperties->bAudioIsEditable = M4OSA_TRUE;
    }

    /**
    * Check there is at least one valid stream in the file... */
    if( video_err != M4NO_ERROR )
        return video_err;

    if( audio_err != M4NO_ERROR )
        return audio_err;

    if( 0 == uiNbOfValidStreams )
    {
        M4OSA_TRACE1_0(
            "M4VSS3GPP_intCheckClipCompatibleWithVssEditing(): File contains no supported stream,\
            returning M4VSS3GPP_ERR_EDITING_NO_SUPPORTED_STREAM_IN_FILE");
        return M4VSS3GPP_ERR_EDITING_NO_SUPPORTED_STREAM_IN_FILE;
    }

    /**
    * Return with no error */
    M4OSA_TRACE3_0(
        "M4VSS3GPP_intCheckClipCompatibleWithVssEditing(): returning M4NO_ERROR");
    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * M4OSA_ERR M4VSS3GPP_intAudioMixingCompatibility()
 * @brief    This function allows checking if two clips are compatible with each other for
 *        VSS 3GPP audio mixing feature.
 * @note
 * @param    pC                            (IN) Context of the audio mixer
 * @param    pInputClipProperties        (IN) Clip analysis of the first clip
 * @param    pAddedClipProperties        (IN) Clip analysis of the second clip
 * @return    M4NO_ERROR:            No error
 * @return    M4ERR_PARAMETER:    At least one parameter is M4OSA_NULL (debug only)
 * @return    M4VSS3GPP_ERR_INVALID_CLIP_ANALYSIS_VERSION
 * @return  M4VSS3GPP_ERR_INPUT_CLIP_IS_NOT_A_3GPP
 * @return  M4NO_ERROR
 ******************************************************************************
 */
M4OSA_ERR
M4VSS3GPP_intAudioMixingCompatibility( M4VSS3GPP_InternalAudioMixingContext
                                      *pC, M4VIDEOEDITING_ClipProperties *pInputClipProperties,
                                      M4VIDEOEDITING_ClipProperties *pAddedClipProperties )
{
    M4OSA_Bool bClip1IsAAC = M4OSA_FALSE;
    M4OSA_Bool bClip2IsAAC = M4OSA_FALSE;

    /**
    * Reset settings */
    pInputClipProperties->bAudioIsEditable = M4OSA_FALSE;
    pAddedClipProperties->bAudioIsEditable = M4OSA_FALSE;
    pInputClipProperties->bAudioIsCompatibleWithMasterClip = M4OSA_FALSE;
    pAddedClipProperties->bAudioIsCompatibleWithMasterClip = M4OSA_FALSE;

    /**
    * Check that analysis has been generated by this version of the VSS3GPP library */
    if( ( pInputClipProperties->Version[0] != M4VIDEOEDITING_VERSION_MAJOR)
        || (pInputClipProperties->Version[1] != M4VIDEOEDITING_VERSION_MINOR)
        || (pInputClipProperties->Version[2]
    != M4VIDEOEDITING_VERSION_REVISION) )
    {
        M4OSA_TRACE1_0(
            "M4VSS3GPP_intAudioMixingCompatibility: The clip analysis has been generated\
            by another version, returning M4VSS3GPP_ERR_INVALID_CLIP_ANALYSIS_VERSION");
        return M4VSS3GPP_ERR_INVALID_CLIP_ANALYSIS_VERSION;
    }

    if( ( pAddedClipProperties->Version[0] != M4VIDEOEDITING_VERSION_MAJOR)
        || (pAddedClipProperties->Version[1] != M4VIDEOEDITING_VERSION_MINOR)
        || (pAddedClipProperties->Version[2]
    != M4VIDEOEDITING_VERSION_REVISION) )
    {
        M4OSA_TRACE1_0(
            "M4VSS3GPP_intAudioMixingCompatibility: The clip analysis has been generated\
            by another version, returning M4VSS3GPP_ERR_INVALID_CLIP_ANALYSIS_VERSION");
        return M4VSS3GPP_ERR_INVALID_CLIP_ANALYSIS_VERSION;
    }

    /********* input file type *********/

    if( M4VIDEOEDITING_kFileType_3GPP != pInputClipProperties->FileType )
    {
        M4OSA_TRACE1_0(
            "M4VSS3GPP_intAudioMixingCompatibility:\
            returning M4VSS3GPP_ERR_INPUT_CLIP_IS_NOT_A_3GPP");
        return M4VSS3GPP_ERR_INPUT_CLIP_IS_NOT_A_3GPP;
    }

    /********* input audio *********/

    if( M4VIDEOEDITING_kNoneAudio != pInputClipProperties->
        AudioStreamType ) /**< if there is an audio stream */
    {
        /**
        * Check audio format is AMR-NB or AAC */
        switch( pInputClipProperties->AudioStreamType )
        {
            case M4VIDEOEDITING_kAMR_NB:
                pInputClipProperties->bAudioIsEditable = M4OSA_TRUE;
                break;

            case M4VIDEOEDITING_kAAC:
            case M4VIDEOEDITING_kAACplus:
            case M4VIDEOEDITING_keAACplus:
                switch( pInputClipProperties->uiSamplingFrequency )
                {
                case 8000:
                case 16000:
                case 22050:
                case 24000:
                case 32000:
                case 44100:
                case 48000:
                    pInputClipProperties->bAudioIsEditable = M4OSA_TRUE;
                    break;

                default:
                    break;
            }
            bClip1IsAAC = M4OSA_TRUE;
            break;
          default:
            break;
        }
    }
    else
    {
        /* Silence is always editable */
        pInputClipProperties->bAudioIsEditable = M4OSA_TRUE;
    }

    /********* added audio *********/

    if( M4VIDEOEDITING_kNoneAudio != pAddedClipProperties->
        AudioStreamType ) /**< if there is an audio stream */
    {
        /**
        * Check audio format is AMR-NB or AAC */
        switch( pAddedClipProperties->AudioStreamType )
        {
            case M4VIDEOEDITING_kAMR_NB:
                pAddedClipProperties->bAudioIsEditable = M4OSA_TRUE;
                pAddedClipProperties->bAudioIsCompatibleWithMasterClip =
                    M4OSA_TRUE; /* I use this field to know if silence supported */
                break;

            case M4VIDEOEDITING_kAAC:
            case M4VIDEOEDITING_kAACplus:
            case M4VIDEOEDITING_keAACplus:
                switch( pAddedClipProperties->uiSamplingFrequency )
                {
                case 8000:
                case 16000:
                case 22050:
                case 24000:
                case 32000:
                case 44100:
                case 48000:
                    pAddedClipProperties->bAudioIsEditable = M4OSA_TRUE;
                    break;

                default:
                    break;
                }
                pAddedClipProperties->bAudioIsCompatibleWithMasterClip =
                    M4OSA_TRUE; /* I use this field to know if silence supported */
                bClip2IsAAC = M4OSA_TRUE;
                break;

            case M4VIDEOEDITING_kEVRC:
                break;

            case M4VIDEOEDITING_kPCM:
                pAddedClipProperties->bAudioIsEditable = M4OSA_TRUE;
                pAddedClipProperties->bAudioIsCompatibleWithMasterClip =
                    M4OSA_TRUE; /* I use this field to know if silence supported */

                if( pAddedClipProperties->uiSamplingFrequency == 16000 )
                {
                    bClip2IsAAC = M4OSA_TRUE;
                }
                break;

            case M4VIDEOEDITING_kMP3: /*RC*/
                pAddedClipProperties->bAudioIsEditable = M4OSA_TRUE;
                pAddedClipProperties->bAudioIsCompatibleWithMasterClip =
                    M4OSA_TRUE; /* I use this field to know if silence supported */
                break;

            default:
                /* The writer cannot write this  into a 3gpp */
                M4OSA_TRACE1_0(
                    "M4VSS3GPP_intAudioMixingCompatibility:\
                    returning M4VSS3GPP_ERR_UNSUPPORTED_ADDED_AUDIO_STREAM");
                return M4VSS3GPP_ERR_UNSUPPORTED_ADDED_AUDIO_STREAM;
        }
    }
    else
    {
        /* Silence is always editable */
        pAddedClipProperties->bAudioIsEditable = M4OSA_TRUE;
        pAddedClipProperties->bAudioIsCompatibleWithMasterClip =
            M4OSA_TRUE; /* I use this field to know if silence supported */
    }

    if( pC->bRemoveOriginal == M4OSA_FALSE )
    {
        if( pInputClipProperties->uiSamplingFrequency
            != pAddedClipProperties->uiSamplingFrequency )
        {
            /* We need to call SSRC in order to align ASF and/or nb of channels */
            /* Moreover, audio encoder may be needed in case of audio replacing... */
            pC->b_SSRCneeded = M4OSA_TRUE;
        }

        if( pInputClipProperties->uiNbChannels
            < pAddedClipProperties->uiNbChannels )
        {
            /* Stereo to Mono */
            pC->ChannelConversion = 1;
        }
        else if( pInputClipProperties->uiNbChannels
            > pAddedClipProperties->uiNbChannels )
        {
            /* Mono to Stereo */
            pC->ChannelConversion = 2;
        }
    }

    pInputClipProperties->bAudioIsCompatibleWithMasterClip = M4OSA_TRUE;

    /**
    * Return with no error */
    M4OSA_TRACE3_0(
        "M4VSS3GPP_intAudioMixingCompatibility(): returning M4NO_ERROR");
    return M4NO_ERROR;
}
