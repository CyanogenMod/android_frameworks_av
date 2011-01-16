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

#include "M4OSA_Types.h"
#include "M4OSA_Debug.h"

#include "M4VD_EXTERNAL_Interface.h"
#include "M4VD_EXTERNAL_Internal.h"
#include "M4VD_Tools.h"

/**
 ************************************************************************
 * @file   M4VD_EXTERNAL_BitstreamParser.c
 * @brief
 * @note   This file implements external Bitstream parser
 ************************************************************************
 */

M4OSA_UInt32 M4VD_EXTERNAL_GetBitsFromMemory(M4VS_Bitstream_ctxt* parsingCtxt,
     M4OSA_UInt32 nb_bits)
{
#if 0
    M4OSA_UInt32    code;
    M4OSA_UInt32    i;

    code = 0;
    for (i = 0; i < nb_bits; i++)
    {
        if (parsingCtxt->stream_index == 8)
        {
            M4OSA_memcpy( (M4OSA_MemAddr8)&(parsingCtxt->stream_byte), parsingCtxt->in,
                 sizeof(unsigned char));
            parsingCtxt->in++;
            //fread(&stream_byte, sizeof(unsigned char),1,in);
            parsingCtxt->stream_index = 0;
        }
        code = (code << 1);
        code |= ((parsingCtxt->stream_byte & 0x80) >> 7);

        parsingCtxt->stream_byte = (parsingCtxt->stream_byte << 1);
        parsingCtxt->stream_index++;
    }

    return code;
#endif
        return(M4VD_Tools_GetBitsFromMemory(parsingCtxt,nb_bits));
}

M4OSA_ERR M4VD_EXTERNAL_WriteBitsToMemory(M4OSA_UInt32 bitsToWrite,
                                                 M4OSA_MemAddr32 dest_bits,
                                                 M4OSA_UInt8 offset, M4OSA_UInt8 nb_bits)
{
#if 0
    M4OSA_UInt8 i,j;
    M4OSA_UInt32 temp_dest = 0, mask = 0, temp = 1;
    M4OSA_UInt32 input = bitsToWrite;

    input = (input << (32 - nb_bits - offset));

    /* Put destination buffer to 0 */
    for(j=0;j<3;j++)
    {
        for(i=0;i<8;i++)
        {
            if((j*8)+i >= offset && (j*8)+i < nb_bits + offset)
            {
                mask |= (temp << ((7*(j+1))-i+j));
            }
        }
    }
    mask = ~mask;
    *dest_bits &= mask;

    /* Parse input bits, and fill output buffer */
    for(j=0;j<3;j++)
    {
        for(i=0;i<8;i++)
        {
            if((j*8)+i >= offset && (j*8)+i < nb_bits + offset)
            {
                temp = ((input & (0x80000000 >> offset)) >> (31-offset));
                //*dest_bits |= (temp << (31 - i));
                *dest_bits |= (temp << ((7*(j+1))-i+j));
                input = (input << 1);
            }
        }
    }

    return M4NO_ERROR;
#endif
        return (M4VD_Tools_WriteBitsToMemory( bitsToWrite,dest_bits,
                                                offset,  nb_bits));
}

M4OSA_ERR M4DECODER_EXTERNAL_ParseVideoDSI(M4OSA_UInt8* pVol, M4OSA_Int32 aVolSize,
                                             M4DECODER_MPEG4_DecoderConfigInfo* pDci,
                                             M4DECODER_VideoSize* pVideoSize)
{
    M4VS_Bitstream_ctxt parsingCtxt;
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
    pVideoSize->m_uiWidth              = 0;
    pVideoSize->m_uiHeight             = 0;

    pDci->uiTimeScale          = 0;
    pDci->uiProfile            = 0;
    pDci->uiUseOfResynchMarker = 0;
    pDci->bDataPartition       = M4OSA_FALSE;
    pDci->bUseOfRVLC           = M4OSA_FALSE;

    /* Reset the bitstream context */
    parsingCtxt.stream_byte = 0;
    parsingCtxt.stream_index = 8;
    parsingCtxt.in = (M4OSA_Int8 *)pVol;

    start = (M4OSA_Int8 *)pVol;

    /* Start parsing */
    while (parsingCtxt.in - start < aVolSize)
    {
        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 8);
        if (code == 0)
        {
            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 8);
            if (code == 0)
            {
                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 8);
                if (code == 1)
                {
                    /* start code found */
                    code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 8);

                    /* ----- 0x20..0x2F : video_object_layer_start_code ----- */

                    if ((code > 0x1F) && (code < 0x30))
                    {
                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                 1);/* random accessible vol */
                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                 8);/* video object type indication */
                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                 1);/* is object layer identifier */
                        if (code == 1)
                        {
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     4); /* video object layer verid */
                            vol_verid = (M4OSA_UInt8)code;
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     3); /* video object layer priority */
                        }
                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                 4);/* aspect ratio */
                        if (code == 15)
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     16); /* par_width and par_height (8+8) */
                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                 1);/* vol control parameters */
                        if (code == 1)
                        {
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     3);/* chroma format + low delay (3+1) */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     1);/* vbv parameters */
                            if (code == 1)
                            {
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         32);/* first and latter half bitrate + 2 marker bits
                                            (15 + 1 + 15 + 1) */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         31);/* first and latter half vbv buffer size + first
                                          half vbv occupancy + marker bits (15+1+3+11+1)*/
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         16);/* first half vbv occupancy + marker bits (15+1)*/
                            }
                        }
                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                 2); /* video object layer shape */
                        /* Need to save it for vop parsing */
                        video_object_layer_shape = (M4OSA_UInt8)code;

                        if (code != 0) return 0; /* only rectangular case supported */

                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                 1); /* Marker bit */
                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                 16); /* VOP time increment resolution */
                        pDci->uiTimeScale = code;

                        /* Computes time increment length */
                        j    = code - 1;
                        for (i = 0; (i < 32) && (j != 0); j >>=1)
                        {
                            i++;
                        }
                        time_incr_length = (i == 0) ? 1 : i;

                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                 1);/* Marker bit */
                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                 1);/* Fixed VOP rate */
                        if (code == 1)
                        {
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     time_incr_length);/* Fixed VOP time increment */
                        }

                        if(video_object_layer_shape != 1) /* 1 = Binary */
                        {
                            if(video_object_layer_shape == 0) /* 0 = rectangular */
                            {
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         1);/* Marker bit */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         13);/* Width */
                                pVideoSize->m_uiWidth = code;
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         1);/* Marker bit */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         13);/* Height */
                                pVideoSize->m_uiHeight = code;
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         1);/* Marker bit */
                            }
                        }

                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                 1);/* interlaced */
                        interlaced = (M4OSA_UInt8)code;
                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                 1);/* OBMC disable */

                        if(vol_verid == 1)
                        {
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     1);/* sprite enable */
                            sprite_enable = (M4OSA_UInt8)code;
                        }
                        else
                        {
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     2);/* sprite enable */
                            sprite_enable = (M4OSA_UInt8)code;
                        }
                        if ((sprite_enable == 1) || (sprite_enable == 2))
                        /* Sprite static = 1 and Sprite GMC = 2 */
                        {
                            if (sprite_enable != 2)
                            {

                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         13);/* sprite width */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         1);/* Marker bit */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         13);/* sprite height */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         1);/* Marker bit */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         13);/* sprite l coordinate */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         1);/* Marker bit */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         13);/* sprite top coordinate */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         1);/* Marker bit */
                            }

                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     6);/* sprite warping points */
                            sprite_warping_points = (M4OSA_UInt8)code;
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     2);/* sprite warping accuracy */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     1);/* sprite brightness change */
                            sprite_brightness_change = (M4OSA_UInt8)code;
                            if (sprite_enable != 2)
                            {
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                             1);/* low latency sprite enable */
                            }
                        }
                        if ((vol_verid != 1) && (video_object_layer_shape != 0))
                        {
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         1);/* sadct disable */
                        }

                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 1); /* not 8 bits */
                        if (code)
                        {
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     4);/* quant precision */
                            quant_precision = (M4OSA_UInt8)code;
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         4);/* bits per pixel */
                        }

                        /* greyscale not supported */
                        if(video_object_layer_shape == 3)
                        {
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     3); /* nogray quant update + composition method +
                                            linear composition */
                        }

                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     1);/* quant type */
                        if (code)
                        {
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         1);/* load intra quant mat */
                            if (code)
                            {
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 8);/* */
                                 i    = 1;
                                while (i < 64)
                                {
                                    code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 8);
                                    if (code == 0)
                                        break;
                                    i++;
                                }
                            }

                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                         1);/* load non intra quant mat */
                            if (code)
                            {
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 8);/* */
                                 i    = 1;
                                while (i < 64)
                                {
                                    code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 8);
                                    if (code == 0)
                                        break;
                                    i++;
                                }
                            }
                        }

                        if (vol_verid != 1)
                        {
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     1);/* quarter sample */
                        }

                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     1);/* complexity estimation disable */
                        complexity_estimation_disable = (M4OSA_UInt8)code;
                        if (!code)
                        {
                            //return M4ERR_NOT_IMPLEMENTED;
                        }

                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     1);/* resync marker disable */
                        pDci->uiUseOfResynchMarker = (code) ? 0 : 1;

                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt,
                                     1);/* data partitionned */
                        pDci->bDataPartition = (code) ? M4OSA_TRUE : M4OSA_FALSE;
                        if (code)
                        {
                            /* reversible VLC */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 1);
                            pDci->bUseOfRVLC = (code) ? M4OSA_TRUE : M4OSA_FALSE;
                        }

                        if (vol_verid != 1)
                        {
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 1);/* newpred */
                            if (code)
                            {
                                //return M4ERR_PARAMETER;
                            }
                            /* reduced resolution vop enable */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 1);
                            reduced_resolution_vop_enable = (M4OSA_UInt8)code;
                        }

                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 1);/* scalability */
                        scalability = (M4OSA_UInt8)code;
                        if (code)
                        {
                            /* hierarchy type */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 1);
                            b_hierarchy_type = (M4OSA_UInt8)code;
                            /* ref layer id */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 4);
                            /* ref sampling direct */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 1);
                            /* hor sampling factor N */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 5);
                            /* hor sampling factor M */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 5);
                            /* vert sampling factor N */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 5);
                            /* vert sampling factor M */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 5);
                            /* enhancement type */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 1);
                            enhancement_type = (M4OSA_UInt8)code;
                            if ((!b_hierarchy_type) && (video_object_layer_shape == 1))
                            {
                                /* use ref shape */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 1);
                                /* use ref texture */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 1);
                                /* shape hor sampling factor N */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 5);
                                /* shape hor sampling factor M */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 5);
                                /* shape vert sampling factor N */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 5);
                                /* shape vert sampling factor M */
                                code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 5);
                            }
                        }
                        break;
                    }

                    /* ----- 0xB0 : visual_object_sequence_start_code ----- */

                    else if(code == 0xB0)
                    {
                        /* profile_and_level_indication */
                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 8);
                        pDci->uiProfile = (M4OSA_UInt8)code;
                    }

                    /* ----- 0xB5 : visual_object_start_code ----- */

                    else if(code == 0xB5)
                    {
                        /* is object layer identifier */
                        code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 1);
                        if (code == 1)
                        {
                             /* visual object verid */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 4);
                            vol_verid = (M4OSA_UInt8)code;
                             /* visual object layer priority */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 3);
                        }
                        else
                        {
                             /* Realign on byte */
                            code = M4VD_EXTERNAL_GetBitsFromMemory(&parsingCtxt, 7);
                            vol_verid = 1;
                        }
                    }

                    /* ----- end ----- */
                }
                else
                {
                    if ((code >> 2) == 0x20)
                    {
                        /* H263 ...-> wrong*/
                        break;
                    }
                }
            }
        }
    }

    return M4NO_ERROR;
}

M4OSA_ERR M4DECODER_EXTERNAL_ParseAVCDSI(M4OSA_UInt8* pDSI, M4OSA_Int32 DSISize,
                                         M4DECODER_AVCProfileLevel *profile)
{
    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_Bool NALSPS_and_Profile0Found = M4OSA_FALSE;
    M4OSA_UInt16 index;
    M4OSA_Bool    constraintSet3;

    /* check for baseline profile */
    for(index = 0; index < (DSISize-1); index++)
    {
        if(((pDSI[index] & 0x1f) == 0x07) && (pDSI[index+1] == 0x42))
        {
            NALSPS_and_Profile0Found = M4OSA_TRUE;
            break;
        }
    }
    if(M4OSA_FALSE == NALSPS_and_Profile0Found)
    {
        M4OSA_TRACE1_1("M4DECODER_EXTERNAL_ParseAVCDSI: index bad = %d", index);
        *profile = M4DECODER_AVC_kProfile_and_Level_Out_Of_Range;
    }
    else
    {
        M4OSA_TRACE1_1("M4DECODER_EXTERNAL_ParseAVCDSI: index = %d", index);
        constraintSet3 = (pDSI[index+2] & 0x10);
        M4OSA_TRACE1_1("M4DECODER_EXTERNAL_ParseAVCDSI: level = %d", pDSI[index+3]);
        switch(pDSI[index+3])
        {
        case 10:
            *profile = M4DECODER_AVC_kProfile_0_Level_1;
            break;
        case 11:
            if(constraintSet3)
                *profile = M4DECODER_AVC_kProfile_0_Level_1b;
            else
                *profile = M4DECODER_AVC_kProfile_0_Level_1_1;
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

