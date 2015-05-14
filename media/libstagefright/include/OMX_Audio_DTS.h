/*
 * Copyright (c) 2008 The Khronos Group Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/*======================================================================*

 DTS, Inc.
 5220 Las Virgenes Road
 Calabasas, CA 91302  USA

 CONFIDENTIAL: CONTAINS CONFIDENTIAL PROPRIETARY INFORMATION OWNED BY
 DTS, INC. AND/OR ITS AFFILIATES (“DTS”), INCLUDING BUT NOT LIMITED TO
 TRADE SECRETS, KNOW-HOW, TECHNICAL AND BUSINESS INFORMATION. USE,
 DISCLOSURE OR DISTRIBUTION OF THE SOFTWARE IN ANY FORM IS LIMITED TO
 SPECIFICALLY AUTHORIZED LICENSEES OF DTS.  ANY UNAUTHORIZED
 DISCLOSURE IS A VIOLATION OF STATE, FEDERAL, AND dtsInt32ERNATIONAL LAWS.
 BOTH CIVIL AND CRIMINAL PENALTIES APPLY.

 DO NOT DUPLICATE.   COPYRIGHT 2010, DTS, INC.  ALL RIGHTS RESERVED.
 UNAUTHORIZED DUPLICATION IS A VIOLATION OF STATE, FEDERAL AND
 dtsInt32ERNATIONAL LAWS.

 ALGORITHMS, DATA STRUCTURES AND METHODS CONTAINED IN THIS SOFTWARE
 MAY BE PROTECTED BY ONE OR MORE PATENTS OR PATENT APPLICATIONS.

 UNLESS OTHERWISE PROVIDED UNDER THE TERMS OF A FULLY-EXECUTED WRITTEN
 AGREEMENT BY AND BETWEEN THE RECIPIENT HEREOF AND DTS, THE FOLLOWING
 TERMS SHALL APPLY TO ANY USE OF THE SOFTWARE (THE “PRODUCT”) AND, AS
 APPLICABLE, ANY RELATED DOCUMENTATION:  (i) ANY USE OF THE PRODUCT
 AND ANY RELATED DOCUMENTATION IS AT THE RECIPIENT’S SOLE RISK:
 (ii) THE PRODUCT AND ANY RELATED DOCUMENTATION ARE PROVIDED “AS IS”
 AND WITHOUT WARRANTY OF ANY KIND AND DTS EXPRESSLY DISCLAIMS ALL
 WARRANTIES, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 PURPOSE, REGARDLESS OF WHETHER DTS KNOWS OR HAS REASON TO KNOW OF THE
 USER’S PARTICULAR NEEDS; (iii) DTS DOES NOT WARRANT THAT THE PRODUCT
 OR ANY RELATED DOCUMENTATION WILL MEET USER’S REQUIREMENTS, OR THAT
 DEFECTS IN THE PRODUCT OR ANY RELATED DOCUMENTATION WILL BE
 CORRECTED; (iv) DTS DOES NOT WARRANT THAT THE OPERATION OF ANY
 HARDWARE OR SOFTWARE ASSOCIATED WITH THIS DOCUMENT WILL BE
 UNdtsInt32ERRUPTED OR ERROR-FREE; AND (v) UNDER NO CIRCUMSTANCES,
 INCLUDING NEGLIGENCE, SHALL DTS OR THE DIRECTORS, OFFICERS, EMPLOYEES,
 OR AGENTS OF DTS, BE LIABLE TO USER FOR ANY INCIDENTAL, INDIRECT,
 SPECIAL, OR CONSEQUENTIAL DAMAGES (INCLUDING BUT NOT LIMITED TO
 DAMAGES FOR LOSS OF BUSINESS PROFITS, BUSINESS dtsInt32ERRUPTION, AND LOSS
 OF BUSINESS INFORMATION) ARISING OUT OF THE USE, MISUSE, OR INABILITY
 TO USE THE PRODUCT OR ANY RELATED DOCUMENTATION.

 UNRELEASED SOFTWARE:
 Please note that this software is "unreleased" software, which means that
 it is not  complete and may have known or unknown problems or malfunctions
 ("bugs").  It is provided for purposes of testing in order to obtain
 feedback, and is not dtsInt32ended to provide a long term solution for any
 customer need.  The software may not work as expected; the documentation
 is incomplete; there are no tutorials or troubleshooting procedures provided.
 The software may not work on all platforms or under all operating systems.
 Not all hardware is supported.  For these reasons, the software should never
 be  used for critical applications where failure would have any serious
 consequence.  We may undertake to fix "bugs" as they are reported, to complete
 the feature set, and to produce documentation for future versions.  We dtsInt32end
 to release new versions (either "beta" or for release), but we cannot
 guarantee to resolve any particular problem in any particular time frame.
 Although we appreciate your feedback and dtsInt32end to undertake to correct "bugs"
 as they are reported, we do not guarantee support  for this version of
 the software for any period whatsoever.
*======================================================================*/


/** @file OMX_Audio_DTS.h - OpenMax IL version 1.1.2, DTS vendor extension
 *  The structures needed by Audio components to exchange
 *  parameters and configuration data with the componenmilts.
 */

#ifndef OMX_Audio_DTS_h
#define OMX_Audio_DTS_h

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* Extension to OMX_AUDIO_DTSCODINGTYPE */
typedef enum
{
    OMX_AUDIO_CodingDTSHD = OMX_AUDIO_CodingVendorStartUnused + 0x1, /**< DTS-HD vendor extension */
} OMX_AUDIO_DTS_CODINGTYPE;


/* Extension to OMX_AUDIO_CHANNELTYPE */
typedef enum
{
    OMX_AUDIO_ChannelLSS    = OMX_AUDIO_ChannelVendorStartUnused + 0x1,/**< Left Surround on Side */
    OMX_AUDIO_ChannelRSS,    /**< Right Surround on Side  */
    OMX_AUDIO_ChannelLC,     /**< Between Left and Centre in front  */
    OMX_AUDIO_ChannelRC,     /**< Between Right and Centre in front  */
    OMX_AUDIO_ChannelLH,     /**< Left Height in front */
    OMX_AUDIO_ChannelCH,     /**< Centre Height in Front  */
    OMX_AUDIO_ChannelRH,     /**< Right Height in front  */
    OMX_AUDIO_ChannelLFE2,   /**< Low Frequency Effects 2 */
    OMX_AUDIO_ChannelLW,     /**< Left on side in front */
    OMX_AUDIO_ChannelRW,     /**< Right on side in front  */
    OMX_AUDIO_ChannelOH,     /**< Over the listeners Head */
    OMX_AUDIO_ChannelLHS,    /**< Left Height on Side */
    OMX_AUDIO_ChannelRHS,    /**< Right Height on Side  */
    OMX_AUDIO_ChannelCHR,    /**< Centre Height in Rear  */
    OMX_AUDIO_ChannelLHR,    /**< Left Height in Rear */
    OMX_AUDIO_ChannelRHR,    /**< Right Height in Rear  */
    OMX_AUDIO_ChannelCLF,    /* < Low Center in Front */
    OMX_AUDIO_ChannelLLF,    /* < Low Left in Front */
    OMX_AUDIO_ChannelRLF,    /* < Low Right in Front */
    OMX_AUDIO_ChannelLT,     /**< Left Total */
    OMX_AUDIO_ChannelRT      /**< Right Total */
} OMX_AUDIO_DTS_CHANNELTYPE;


typedef enum OMX_AUDIO_DTS_SPKROUTTYPE
{
    OMX_AUDIO_DTSSPKROUT_MASK_NATIVE    = 0x00000,
    OMX_AUDIO_DTSSPKROUT_MASK_C         = 0x00001,
    OMX_AUDIO_DTSSPKROUT_MASK_LR        = 0x00002,
    OMX_AUDIO_DTSSPKROUT_MASK_LsRs      = 0x00004,
    OMX_AUDIO_DTSSPKROUT_MASK_LFE1      = 0x00008,
    OMX_AUDIO_DTSSPKROUT_MASK_Cs        = 0x00010,
    OMX_AUDIO_DTSSPKROUT_MASK_LhRh      = 0x00020,
    OMX_AUDIO_DTSSPKROUT_MASK_LsrRsr    = 0x00040,
    OMX_AUDIO_DTSSPKROUT_MASK_Ch        = 0x00080,
    OMX_AUDIO_DTSSPKROUT_MASK_Oh        = 0x00100,
    OMX_AUDIO_DTSSPKROUT_MASK_LcRc      = 0x00200,
    OMX_AUDIO_DTSSPKROUT_MASK_LwRw      = 0x00400,
    OMX_AUDIO_DTSSPKROUT_MASK_LssRss    = 0x00800,
    OMX_AUDIO_DTSSPKROUT_MASK_LFE_2     = 0x01000,
    OMX_AUDIO_DTSSPKROUT_MASK_LhsRhs    = 0x02000,
    OMX_AUDIO_DTSSPKROUT_MASK_Chr       = 0x04000,
    OMX_AUDIO_DTSSPKROUT_MASK_LhrRhr    = 0x08000,
    OMX_AUDIO_DTSSPKROUT_MASK_Clf       = 0x10000,
    OMX_AUDIO_DTSSPKROUT_MASK_LlfRlf    = 0x20000,
    OMX_AUDIO_DTSSPKROUT_MASK_LtRt      = 0x40000
} OMX_AUDIO_DTS_SPKROUTTYPE;


typedef enum
{
    OMX_AUDIO_DTSSPEAKERMASK_CENTRE = 0x00000001,   /**< Centre */
    OMX_AUDIO_DTSSPEAKERMASK_LEFT   = 0x00000002,   /**< Left  */
    OMX_AUDIO_DTSSPEAKERMASK_RIGHT  = 0x00000004,   /**< Right  */
    OMX_AUDIO_DTSSPEAKERMASK_LS     = 0x00000008,   /**< Left Surround */
    OMX_AUDIO_DTSSPEAKERMASK_RS     = 0x00000010,   /**< Right Surround  */
    OMX_AUDIO_DTSSPEAKERMASK_LFE1   = 0x00000020,   /**< Low Frequency Effects 1 */
    OMX_AUDIO_DTSSPEAKERMASK_Cs     = 0x00000040,   /**< Center Surround  */
    OMX_AUDIO_DTSSPEAKERMASK_Lsr    = 0x00000080,   /**< Left Surround in Rear  */
    OMX_AUDIO_DTSSPEAKERMASK_Rsr    = 0x00000100,   /**< Right Surround in Rear  */
    OMX_AUDIO_DTSSPEAKERMASK_Lss    = 0x00000200,   /**< Left Surround on Side */
    OMX_AUDIO_DTSSPEAKERMASK_Rss    = 0x00000400,   /**< Right Surround on Side  */
    OMX_AUDIO_DTSSPEAKERMASK_Lc     = 0x00000800,   /**< Between Left and Centre in front  */
    OMX_AUDIO_DTSSPEAKERMASK_Rc     = 0x00001000,   /**< Between Right and Centre in front  */
    OMX_AUDIO_DTSSPEAKERMASK_Lh     = 0x00002000,   /**< Left Height in front */
    OMX_AUDIO_DTSSPEAKERMASK_Ch     = 0x00004000,   /**< Centre Height in Front  */
    OMX_AUDIO_DTSSPEAKERMASK_Rh     = 0x00008000,   /**< Right Height in front  */
    OMX_AUDIO_DTSSPEAKERMASK_LFE2   = 0x00010000,   /**< Low Frequency Effects 2 */
    OMX_AUDIO_DTSSPEAKERMASK_Lw     = 0x00020000,   /**< Left on side in front */
    OMX_AUDIO_DTSSPEAKERMASK_Rw     = 0x00040000,   /**< Right on side in front  */
    OMX_AUDIO_DTSSPEAKERMASK_Oh     = 0x00080000,   /**< Over the listeners Head */
    OMX_AUDIO_DTSSPEAKERMASK_Lhs    = 0x00100000,   /**< Left Height on Side */
    OMX_AUDIO_DTSSPEAKERMASK_Rhs    = 0x00200000,   /**< Right Height on Side  */
    OMX_AUDIO_DTSSPEAKERMASK_Chr    = 0x00400000,   /**< Centre Height in Rear  */
    OMX_AUDIO_DTSSPEAKERMASK_Lhr    = 0x00800000,   /**< Left Height in Rear */
    OMX_AUDIO_DTSSPEAKERMASK_Rhr    = 0x01000000,   /**< Right Height in Rear  */
    OMX_AUDIO_DTSSPEAKERMASK_Clf    = 0x02000000,   /**< Low Center in Front */
    OMX_AUDIO_DTSSPEAKERMASK_Llf    = 0x04000000,   /**< Low Left in Front */
    OMX_AUDIO_DTSSPEAKERMASK_Rlf    = 0x08000000,   /**< Low Right in Front */
    OMX_AUDIO_DTSSPEAKERMASK_Lt     = 0x10000000,
    OMX_AUDIO_DTSSPEAKERMASK_Rt     = 0x20000000
} OMX_AUDIO_DTS_SPEAKERTYPE;


/* Extension to OMX_INDEXTYPE */
enum
{
    OMX_IndexParamAudioDTSDec   = OMX_IndexVendorStartUnused + 0x1,     /**< reference: OMX_AUDIO_PARAM_DTSDECTYPE */
#ifndef NOCODEFOR_CDEC_DSEC
    OMX_IndexParamAudioDTSDecKey = OMX_IndexVendorStartUnused + 0x2,
#endif /* #ifndef NOCODEFOR_CDEC_DSEC */
};


typedef struct OMX_AUDIO_PARAM_DTSDECTYPE
{
    OMX_U32 nSize;                          /**< size of the structure in bytes */
    OMX_VERSIONTYPE nVersion;               /**< OMX specification version information */
    OMX_U32 nPortIndex;                     /**< Port index indicating which port to set. Must be input port. */

    /* directive variables */

    OMX_U32 nExSSID;                        /**< ExSS Id from which audio presentation is selected, 0~3. Default is 0. */
    OMX_U32 nAudioPresentIndex;             /**< Audio presentation index, 0~7. Default is 0. */
    OMX_U32 nDRCPercent;                    /**< Percentage of DRC to be applied, 0~100. Default is 0. */
    OMX_BOOL bDialNorm;                     /**< Enable or disable dialog normalization. Default is OMX_TRUE. */
    OMX_AUDIO_DTS_SPKROUTTYPE nSpkrOut;     /**< Requested speaker mask. Default is DTSSPKROUT_MASK_LR.
                                                 Actual output speaker mask depends on input bit stream, may be different from this requested one.
                                                 See OMX_GetParameter, OMX_IndexParamAudioPcm and OMX_AUDIO_PARAM_PCMMODETYPE. */
    OMX_BOOL bMixLFEIntoFront;              /**< Enable or disable mixing LFE into front channels. Default is OMX_FALSE. */
    OMX_U32 nOutputBitWidth ;               /**< Output PCM bit width, 24 or 16. Default is 16.
                                                 It determines nBitPerSample in OMX_AUDIO_PARAM_PCMMODETYPE */

    /* informative variables, read only */

    OMX_U32 nMaxSampleRate;                 /**< Maximum sample rate of the stream. Decoded sample rate is reported in OMX_AUDIO_PARAM_PCMMODETYPE */
    OMX_U32 nSamplesInFrameAtMaxSR;         /**< Frame size at maximum sample rate. */
    OMX_U32 nMaxNumChannels;                /**< Maximum number of channels in the stream. Decoded channles are reported in OMX_AUDIO_PARAM_PCMMODETYPE */
    OMX_AUDIO_DTS_SPEAKERTYPE nMaxChannelMask; /**< Maximum channel mask in the stream. See OMX_AUDIO_DTS_SPEAKERTYPE */
    OMX_U32 nRepTypes;                      /** Representation type, 1: unmapped, 2:LT_RT, 4:LH_RH */
} OMX_AUDIO_PARAM_DTSDECTYPE;

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
/* File EOF */
