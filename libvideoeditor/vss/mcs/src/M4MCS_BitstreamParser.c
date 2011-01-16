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
 *************************************************************************
 * @file   M4MCS_Parsing.c
 * @brief  MCS implementation (Video Compressor Service)
 * @note   This file implements the VOL parsing and timescale 'on the fly' modification
 *************************************************************************
 **/

/**
 ********************************************************************
 * Includes
 ********************************************************************
 */

/* Core headers */
#include "M4MCS_API.h"
#include "M4MCS_InternalTypes.h"
#include "M4VD_Tools.h"


#ifdef TIMESCALE_BUG

/*typedef struct
{
    M4OSA_UInt32 stream_byte;
    M4OSA_UInt32 stream_index;
    M4OSA_MemAddr8 in;

} M4MCS_Bitstream_ctxt;*/
typedef M4VS_Bitstream_ctxt M4MCS_Bitstream_ctxt;

/*
 ************************************************************************
 * M4OSA_UInt32 M4MCS_GetBitsFromMemory( )
 * @brief
 * @return
 ************************************************************************
 */
static M4OSA_UInt32 M4MCS_GetBitsFromMemory(
                                    M4MCS_Bitstream_ctxt* parsingCtxt,
                                    M4OSA_UInt32 nb_bits)
{
    return(M4VD_Tools_GetBitsFromMemory((M4VS_Bitstream_ctxt*) parsingCtxt, nb_bits));
}

/**
 ***********************************************************************
 * M4OSA_ERR M4MCS_WriteBitsToMemory( )
 * @brief
 * @return
 ***********************************************************************
 */
static M4OSA_ERR M4MCS_WriteBitsToMemory(   M4OSA_UInt32 bitsToWrite,
                                            M4OSA_MemAddr32 dest_bits,
                                            M4OSA_UInt8 offset,
                                            M4OSA_UInt8 nb_bits)
{
    return (M4VD_Tools_WriteBitsToMemory(bitsToWrite,
                                         dest_bits,
                                         offset, nb_bits));
}

/**
 ************************************************************************
 * M4OSA_ERR M4MCS_WriteByteToMemory( )
 * @brief
 * @return
 ************************************************************************
 */
static M4OSA_ERR M4MCS_WriteByteToMemory(   M4OSA_UInt8 BytesToWrite,
                                            M4OSA_MemAddr8 dest_bytes)
{
    M4OSA_MemAddr8 addr = dest_bytes;

    *addr = BytesToWrite;

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * M4OSA_Void M4MCS_intCheckIndex( )
 * @brief :
 * @note :    This function can be used to write until 15 bits ...
 *            Depending on the bits offset, it increases or not the 8 bits pointer.
 *            It must be called if more than 8 bits have to be written not consequently.
 * @return
 ************************************************************************
 */
static M4OSA_Void M4MCS_intCheckIndex(  M4OSA_UInt8 *index,
                                        M4OSA_UInt32 a,
                                        M4OSA_MemAddr8* in)
{
    M4OSA_UInt32 offset = a;

    if(offset > 8 && offset <=16)
    {
        offset-=8;
        (*in)++;
    }
    if((*index+offset) >= 8)
    {
        *index = (*index+offset)-8;
        (*in)++;
    }
    else
    {
        *index += offset;
    }
}

/**
 ************************************************************************
 * M4OSA_ERR M4MCS_intParseVideoDSI( )
 * @brief :  This function parses video DSI and changes writer vop time increment resolution
 * @note  :  It also calculates the number of bits on which the vop_time_increment is coded in
 *           the input stream
 * @return
 ************************************************************************
 */
M4OSA_ERR M4MCS_intParseVideoDSI(M4MCS_InternalContext* pC)
{
    M4MCS_Bitstream_ctxt parsingCtxt;
    M4OSA_UInt32 code,j;
    M4OSA_MemAddr8 start, in;
    M4OSA_UInt8 i;
    M4OSA_UInt32 time_incr_length, new_time_incr_length;
    M4OSA_UInt8 vol_verid=0, b_hierarchy_type;

    /* Fill default values */
    pC->volParsing.video_object_layer_shape = 0;
    pC->volParsing.sprite_enable = 0;
    pC->volParsing.reduced_resolution_vop_enable = 0;
    pC->volParsing.scalability = 0;
    pC->volParsing.enhancement_type = 0;
    pC->volParsing.complexity_estimation_disable = 0;
    pC->volParsing.interlaced = 0;
    pC->volParsing.sprite_warping_points = 0;
    pC->volParsing.sprite_brightness_change = 0;
    pC->volParsing.quant_precision = 5;

    parsingCtxt.stream_byte = 0;
    parsingCtxt.stream_index = 8;
    parsingCtxt.in = pC->WriterVideoStreamInfo.Header.pBuf;

    start = pC->WriterVideoStreamInfo.Header.pBuf;

    while (parsingCtxt.in - start\
         < pC->pReaderVideoStream->m_basicProperties.m_decoderSpecificInfoSize)
    {
        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);
        if (code == 0)
        {
            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);
            if (code == 0)
            {
                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);
                if (code == 1)
                {
                    /* start code found */
                    code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);
                    if(code == 0xB5) /* Visual object start code */
                    {
                        /* is object layer identifier */
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                        if (code == 1)
                        {
                            /* visual object verid */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 4);
                            vol_verid = code;
                            /* visual object layer priority */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 3);
                        }
                        else
                        {
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 7); /* Realign on byte */
                            vol_verid = 1;
                        }
                    }
                    else if ((code > 0x1F) && (code < 0x30))
                    { /* find vol start code */
                        /* random accessible vol */
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);

                        /* video object type indication */
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);

                        /* is object layer identifier */
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                        if (code == 1)
                        {
                            /* video object layer verid */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 4);
                            vol_verid = code;
                            /* video object layer priority */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 3);
                        }
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 4);/* aspect ratio */
                        if (code == 15)
                            /* par_width and par_height (8+8) */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 16);
                        /* vol control parameters */
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                        if (code == 1)
                        {
                            /* chroma format + low delay (3+1) */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 3);
                            /* vbv parameters */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                            if (code == 1)
                            {
                                /* first and latter half bitrate + 2 marker bits
                                  (15 + 1 + 15 + 1)*/
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 32);

                                /* first and latter half vbv buffer size + first half
                                   vbv occupancy
                                + marker bits (15+1+3+11+1)*/
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 31);

                                /* first half vbv occupancy + marker bits (15+1)*/
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 16);
                            }
                        }
                        /* video object layer shape */
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 2);

                        /* Need to save it for vop parsing */
                        pC->volParsing.video_object_layer_shape = code;

                        if (code != 0) return 0; /* only rectangular case supported */
                        /* Marker bit */
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                        /* VOP time increment resolution */
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 16);

                        /* Computes time increment length */
                        j    = code - 1;
                        for (i = 0; (i < 32) && (j != 0); j >>=1)
                        {
                            i++;
                        }
                        time_incr_length = (i == 0) ? 1 : i;
                        /* Save time increment length and original timescale */
                        pC->uiOrigTimescaleLength = time_incr_length;
                        pC->uiOrigVideoTimescale = code;

                        /* Compute new time increment length */
                        j    = pC->uiVideoTimescale - 1;
                        for (i = 0; (i < 32) && (j != 0); j >>=1)
                        {
                            i++;
                        }
                        time_incr_length = (i == 0) ? 1 : i;
                        /* Save new time increment length */
                        pC->uiTimescaleLength = time_incr_length;

                        /* Write new VOP time increment resolution */
                        if(parsingCtxt.stream_index == 0)
                        {
                            in = parsingCtxt.in - 2;
                        }
                        else
                        {
                            in = parsingCtxt.in - 3;
                        }
                        M4MCS_WriteByteToMemory(pC->uiVideoTimescale, in,
                            parsingCtxt.stream_index, 16 );

                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);/* Marker bit */
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);/* Fixed VOP rate */
                        if (code == 1)
                        {
                            /* Fixed VOP time increment resolution */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt,
                                    time_incr_length);
                        }

                        if(pC->volParsing.video_object_layer_shape != 1) /* 1 = Binary */
                        {
                            if(pC->volParsing.video_object_layer_shape == 0) /* 0 = rectangular */
                            {
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);/* Marker bit */
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 13);/* Width */
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);/* Marker bit */
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 13);/* Height */
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);/* Marker bit */
                            }
                        }

                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);/* interlaced */
                        pC->volParsing.interlaced = code;
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);/* OBMC disable */

                        if(vol_verid == 1)
                        {
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);/* sprite enable */
                            pC->volParsing.sprite_enable = code;
                        }
                        else
                        {
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 2);/* sprite enable */
                            pC->volParsing.sprite_enable = code;
                        }
                        if ((pC->volParsing.sprite_enable == 1) ||
                            (pC->volParsing.sprite_enable == 2))
                            /* Sprite static = 1 and Sprite GMC = 2 */
                        {
                            if (pC->volParsing.sprite_enable != 2)
                            {
                                /* sprite width */
                                code = M4MCS_GetBitsFromMemory(
                                        &parsingCtxt, 13);
                                code = M4MCS_GetBitsFromMemory(
                                        &parsingCtxt, 1);/* Marker bit */
                                /* sprite height */
                                code = M4MCS_GetBitsFromMemory(
                                        &parsingCtxt, 13);
                                code = M4MCS_GetBitsFromMemory(
                                        &parsingCtxt, 1);/* Marker bit */
                                /* sprite l coordinate */
                                code = M4MCS_GetBitsFromMemory(
                                        &parsingCtxt, 13);
                                code = M4MCS_GetBitsFromMemory(
                                        &parsingCtxt, 1);/* Marker bit */
                                /* sprite top coordinate */
                                code = M4MCS_GetBitsFromMemory(
                                        &parsingCtxt, 13);
                                code = M4MCS_GetBitsFromMemory(
                                        &parsingCtxt, 1);/* Marker bit */
                            }
                            /* sprite warping points */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 6);
                            pC->volParsing.sprite_warping_points = code;
                            /* sprite warping accuracy */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 2);

                            /* sprite brightness change */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                            pC->volParsing.sprite_brightness_change = code;
                            if (pC->volParsing.sprite_enable != 2)
                            {
                                /* low latency sprite enable */
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                            }
                        }
                        if ((vol_verid != 1) && (pC->volParsing.video_object_layer_shape != 0))
                        {
                            code = M4MCS_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* sadct disable */
                        }

                        code = M4MCS_GetBitsFromMemory(
                                &parsingCtxt, 1); /* not 8 bits */
                        if (code)
                        {   /* quant precision */
                            code = M4MCS_GetBitsFromMemory(
                                    &parsingCtxt, 4);
                            pC->volParsing.quant_precision = code;
                            /* bits per pixel */
                            code = M4MCS_GetBitsFromMemory(
                                    &parsingCtxt, 4);
                        }

                        /* greyscale not supported */
                        if(pC->volParsing.video_object_layer_shape == 3)
                        {
                            /* nogray quant update + composition method + linear composition */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 3);
                        }

                        code = M4MCS_GetBitsFromMemory(
                                &parsingCtxt, 1);/* quant type */
                        if (code)
                        {
                            /* load intra quant mat */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                            if (code)
                            {
                                code = M4MCS_GetBitsFromMemory(
                                        &parsingCtxt, 8);
                                i    = 1;
                                while (i < 64)
                                {
                                    code = M4MCS_GetBitsFromMemory(
                                            &parsingCtxt, 8);
                                    if (code == 0)
                                        break;
                                    i++;
                                }
                            }
                            /* load non intra quant mat */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                            if (code)
                            {
                                code = M4MCS_GetBitsFromMemory(
                                        &parsingCtxt, 8);
                                i    = 1;
                                while (i < 64)
                                {
                                    code = M4MCS_GetBitsFromMemory(
                                            &parsingCtxt, 8);
                                    if (code == 0)
                                        break;
                                    i++;
                                }
                            }
                        }

                        if (vol_verid != 1)
                        {
                            code = M4MCS_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* quarter sample */
                        }
                        /* complexity estimation disable */
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                        pC->volParsing.complexity_estimation_disable = code;
                        if (!code)
                        {
                            return M4ERR_NOT_IMPLEMENTED;
                        }

                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);/* resync marker disable*/

                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);/* data partitionned */
                        if (code)
                        {
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);/* reversible VLC */
                        }

                        if (vol_verid != 1)
                        {
                            code = M4MCS_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* newpred */
                            if (code)
                            {
                                return M4ERR_PARAMETER;
                            }
                            /* reduced resolution vop enable */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                            pC->volParsing.reduced_resolution_vop_enable = code;
                        }

                        code = M4MCS_GetBitsFromMemory(
                                &parsingCtxt, 1);/* scalability */
                        pC->volParsing.scalability = code;
                        if (code)
                        {
                            code = M4MCS_GetBitsFromMemory(
                                    &parsingCtxt, 1);/* hierarchy type */
                            b_hierarchy_type = code;
                            code = M4MCS_GetBitsFromMemory(
                                    &parsingCtxt, 4);/* ref layer id */

                            /* ref sampling direct */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);

                            /* hor sampling factor N */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 5);

                            /* hor sampling factor M */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 5);

                            /* vert sampling factor N */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 5);

                            /* vert sampling factor M */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 5);

                            /* enhancement type */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                            pC->volParsing.enhancement_type = code;
                            if ((!b_hierarchy_type) &&
                                (pC->volParsing.video_object_layer_shape == 1))
                            {
                                /* use ref shape */
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                                /* use ref texture */
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                                /* shape hor sampling factor N */
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 5);
                                /* shape hor sampling factor M */
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 5);
                                /* shape vert sampling factor N */
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 5);
                                /* shape vert sampling factor M */
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 5);
                            }
                        }
                        break;
                    }
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

/**
 ************************************************************************
 * M4OSA_ERR M4MCS_intChangeAUVideoTimescale( )
 * @brief
 * @return
 ************************************************************************
 */
M4OSA_ERR M4MCS_intChangeAUVideoTimescale(M4MCS_InternalContext* pC)
{
    M4MCS_Bitstream_ctxt parsingCtxt;
    M4OSA_UInt32 code;
    M4OSA_MemAddr8 start, in;
    M4OSA_MemAddr32 in_temp;
    M4OSA_UInt8 i, in_index=0; /* in_index is the bit index in the input buffer */
    M4OSA_UInt32 new_time_incr;
    M4OSA_Int32 diff_timescale= 0 ;
    M4OSA_UInt32 stuffing_byte=0;
    M4OSA_UInt8 vopCodingType, vop_fcode_forward, vop_fcode_backward, nb_zeros;

    parsingCtxt.stream_byte = 0;
    parsingCtxt.stream_index = 8;
    parsingCtxt.in = pC->ReaderVideoAU.m_dataAddress;

    start = pC->ReaderVideoAU.m_dataAddress;
    in = pC->WriterVideoAU.dataAddress;

    M4OSA_memset(in, pC->ReaderVideoAU.m_size , 0);
    code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);
    M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, 0, 8);
    in++;
    if (code == 0)
    {
        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);
        M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, 0, 8);
        in++;
        if (code == 0)
        {
            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);
            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, 0, 8);
            in++;
            if (code == 1)
            {
                /* start code found */
                code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);
                M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, 0, 8);
                in++;
                if (code == 0xB6)
                { /* find vop start code */
                    code = M4MCS_GetBitsFromMemory(&parsingCtxt, 2); /* VOP coding type */
                    M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in,
                                            in_index, 2);
                    M4MCS_intCheckIndex(&in_index,2,&in);
                    vopCodingType = code; /* Save it before needed further in parsing */
                    do
                    {
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1); /* Modulo time base */
                        M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in,
                                                in_index, 1);
                        M4MCS_intCheckIndex(&in_index,1,&in);
                    } while(code != 0);
                    code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1); /* Marker bit */
                    M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in,
                                            in_index, 1);
                    M4MCS_intCheckIndex(&in_index,1,&in);
                    code = M4MCS_GetBitsFromMemory(&parsingCtxt,
                                            pC->uiOrigTimescaleLength);
                    /* VOP time increment */

                    /* Calculates new time increment and write it to AU */
                    new_time_incr = (pC->uiVideoTimescale * code) /
                                    pC->uiOrigVideoTimescale;
                    M4MCS_WriteByteToMemory(new_time_incr, in,
                                    in_index, pC->uiTimescaleLength );
                    M4MCS_intCheckIndex(&in_index,pC->uiTimescaleLength,
                                    &in);

                    /* VOP not coded */
                    code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1); /* Marker bit */
                    M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in,
                                    in_index, 1);
                    M4MCS_intCheckIndex(&in_index,1,&in);
                    code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1); /* VOP not coded bit */
                    M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in,
                                    in_index, 1);
                    M4MCS_intCheckIndex(&in_index,1,&in);
                    if(code == 1)
                    {
                        //break; /* TODO !!! -> Goto stuffing */
                    }
                    /* newpred ignored */

                    if((pC->volParsing.video_object_layer_shape != 2) &&
                        (vopCodingType == 1 || vopCodingType == 3 &&
                        pC->volParsing.sprite_enable == 2))
                    {
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1); /* VOP rounding type */
                        M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in,
                                    in_index, 1);
                        M4MCS_intCheckIndex(&in_index,1,&in);
                    }

                    if(pC->volParsing.reduced_resolution_vop_enable &&
                        pC->volParsing.video_object_layer_shape == 0 &&
                        (vopCodingType == 0 || vopCodingType == 1))
                    {
                        /* VOP reduced resolution */
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                        M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in,
                                    in_index, 1);
                        M4MCS_intCheckIndex(&in_index,1,&in);
                    }

                    if(pC->volParsing.video_object_layer_shape != 0)
                    {
                        if(pC->volParsing.sprite_enable == 1 &&
                            vopCodingType == 0)
                        {
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 13); /* VOP width */
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 13);
                            M4MCS_intCheckIndex(&in_index,13,&in);
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1); /* Marker bit */
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 1);
                            M4MCS_intCheckIndex(&in_index,1,&in);
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 13); /* VOP height */
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 13);
                            M4MCS_intCheckIndex(&in_index,13,&in);
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1); /* Marker bit */
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 1);
                            M4MCS_intCheckIndex(&in_index,1,&in);
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 13); /* VOP horizontal
                                                                              mc spatial ref */
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 13);
                            M4MCS_intCheckIndex(&in_index,13,&in);
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1); /* Marker bit */
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 1);
                            M4MCS_intCheckIndex(&in_index,1,&in);
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 13); /* VOP vertical
                                                                              mc spatial ref */
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 13);
                            M4MCS_intCheckIndex(&in_index,13,&in);
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1); /* Marker bit */
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 1);
                            M4MCS_intCheckIndex(&in_index,1,&in);
                        }
                        if(pC->volParsing.video_object_layer_shape != 1 &&
                            pC->volParsing.scalability &&
                            pC->volParsing.enhancement_type)
                        {
                            /* Background composition */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);

                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 1);
                            M4MCS_intCheckIndex(&in_index,1,&in);
                        }
                        /* Change conv ratio disable */
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);

                        M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 1);
                        M4MCS_intCheckIndex(&in_index,1,&in);
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1); /* VOP constant alpha */
                        M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 1);
                        M4MCS_intCheckIndex(&in_index,1,&in);
                        if(code)
                        {
                            /* VOP constant alpha value */
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);

                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 8);
                            M4MCS_intCheckIndex(&in_index,8,&in);
                        }
                    }

                    if(pC->volParsing.video_object_layer_shape != 2)
                    {
                        if(!pC->volParsing.complexity_estimation_disable)
                        {
                            return M4ERR_NOT_IMPLEMENTED;
                        }
                    }

                    if(pC->volParsing.video_object_layer_shape != 2)
                    {
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 3); /* intra dc vlc thr */
                        M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 3);
                        M4MCS_intCheckIndex(&in_index,3,&in);
                        if(pC->volParsing.interlaced)
                        {
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1); /* top field first */
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 1);
                            M4MCS_intCheckIndex(&in_index,1,&in);
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1); /* alternate vertical
                                                                             scan flag */
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 1);
                            M4MCS_intCheckIndex(&in_index,1,&in);
                        }
                    }

                    if((pC->volParsing.sprite_enable == 1 || pC->volParsing.sprite_enable == 2) &&
                        vopCodingType == 3)
                    {
                        if(pC->volParsing.sprite_warping_points > 0 ||
                            (pC->volParsing.sprite_brightness_change))
                        {
                            return M4ERR_NOT_IMPLEMENTED;
                        }
                        if(pC->volParsing.sprite_enable == 1)
                        {
                            return M4ERR_NOT_IMPLEMENTED;
                        }
                    }

                    if(pC->volParsing.video_object_layer_shape != 2)
                    {
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt,
                            pC->volParsing.quant_precision); /* vop_quant */
                        M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index,
                            pC->volParsing.quant_precision);
                        M4MCS_intCheckIndex(&in_index,pC->volParsing.quant_precision,&in);
                        if(pC->volParsing.video_object_layer_shape == 3)
                        {
                            return M4ERR_NOT_IMPLEMENTED;
                        }
                        if(vopCodingType != 0) /* P-VOP or S-VOP or B-VOP case */
                        {
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 3); /* vop fcode forward*/
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 3);
                            M4MCS_intCheckIndex(&in_index,3,&in);
                            vop_fcode_forward = code;
                        }
                        if(vopCodingType == 2) /* B-VOP */
                        {
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 3); /* vop fcode forward*/
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 3);
                            M4MCS_intCheckIndex(&in_index,3,&in);
                            vop_fcode_backward = code;
                        }

                    }

#if 1
                    /* Align on read */
                    code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8-(parsingCtxt.stream_index));
                    M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in,
                        in_index, 8-(parsingCtxt.stream_index));
                    M4MCS_intCheckIndex(&in_index,8-(parsingCtxt.stream_index),&in);

                    do
                    {
                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);
                        if(code == 0)
                        {
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);
                            if(code == 0)
                            {
                                nb_zeros = 0;
                                if((vopCodingType == 1 || vopCodingType == 3)
                                    && vop_fcode_forward > 1) /* P-VOP or S-VOP case */
                                {
                                    code = M4MCS_GetBitsFromMemory(&parsingCtxt,
                                        vop_fcode_forward-1);
                                    nb_zeros = vop_fcode_forward-1;
                                }
                                else if(vopCodingType == 2 && (vop_fcode_forward > 1 ||
                                    vop_fcode_backward > 1)) /* B-VOP case */
                                {
                                    if(vop_fcode_forward > vop_fcode_backward)
                                    {
                                        if(15+vop_fcode_forward > 17)
                                        {
                                            code = M4MCS_GetBitsFromMemory(&parsingCtxt,
                                                vop_fcode_forward-1);
                                        }
                                        else
                                        {
                                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                                        }
                                        nb_zeros = vop_fcode_forward-1;
                                    }
                                    else
                                    {
                                        if(15+vop_fcode_backward > 17)
                                        {
                                            code = M4MCS_GetBitsFromMemory(&parsingCtxt,
                                                vop_fcode_backward-1);
                                        }
                                        else
                                        {
                                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                                        }
                                        nb_zeros = vop_fcode_backward-1;
                                    }
                                    if(code == 0)
                                    {
                                        code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                                        if(code != 1)
                                        {
                                            M4MCS_WriteByteToMemory(0, (M4OSA_MemAddr32)in,
                                                in_index, 8);
                                            M4MCS_intCheckIndex(&in_index,8,&in);
                                            M4MCS_WriteByteToMemory(0, (M4OSA_MemAddr32)in,
                                                in_index, 8);
                                            M4MCS_intCheckIndex(&in_index,8,&in);
                                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in,
                                                in_index, 1);
                                            M4MCS_intCheckIndex(&in_index,1,&in);
                                            goto realign;
                                        }
                                        else
                                        {
                                            M4MCS_intChangeVideoPacketVideoTimescale(pC );
                                        }
                                    }
                                    else
                                    {

                                        goto realign;
                                    }
                                }
                                else /* I-VOP case or P-VOP or S-VOP case with
                                     vop_fcode_forward = 1 */
                                {
                                    /* Read next bit that must be one */
                                    code = M4MCS_GetBitsFromMemory(&parsingCtxt, 1);
                                    if(code != 1)
                                    {
                                        goto realign;
                                    }
                                    else
                                    {
                                        /* Realign on byte */

                                        /* Write resync marker */
                                        M4MCS_WriteByteToMemory(0, (M4OSA_MemAddr32)in,
                                            in_index, 8);
                                        M4MCS_intCheckIndex(&in_index,8,&in);
                                        M4MCS_WriteByteToMemory(0, (M4OSA_MemAddr32)in,
                                            in_index, 8);
                                        M4MCS_intCheckIndex(&in_index,8,&in);
                                        M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in,
                                            in_index, 1);
                                        M4MCS_intCheckIndex(&in_index,1,&in);

                                        /* Change timescale into video packet header */
                                        M4MCS_intChangeVideoPacketVideoTimescale(pC );
                                    }

                                }
                            }
                            else
                            {
                                M4MCS_WriteByteToMemory(0, (M4OSA_MemAddr32)in, in_index, 8);
                                M4MCS_intCheckIndex(&in_index,8,&in);
                                M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 8);
                                M4MCS_intCheckIndex(&in_index,8,&in);
realign:
                                /* Realign on read */
                                code = M4MCS_GetBitsFromMemory(&parsingCtxt,
                                    8-(parsingCtxt.stream_index));
                                M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index,
                                    8-(parsingCtxt.stream_index));
                                M4MCS_intCheckIndex(&in_index,8-(parsingCtxt.stream_index),&in);
                            }
                        }
                        else
                        {
                            M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 8);
                            M4MCS_intCheckIndex(&in_index,8,&in);
                        }
                    } while(parsingCtxt.in - pC->ReaderVideoAU.m_dataAddress\
                        < pC->ReaderVideoAU.m_size);
#else
                    /* Align on write */
                    code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8-in_index);
                    M4MCS_WriteByteToMemory(code, (M4OSA_MemAddr32)in, in_index, 8-in_index);
                    M4MCS_intCheckIndex(&in_index,8-in_index,&in);

                    /* Read 8 bits words, and write them to the output AU
                    (write is 8 bits aligned) */
                    diff_timescale = pC->uiOrigTimescaleLength - pC->uiTimescaleLength;
                    if(diff_timescale > 0)
                    {
                        while (parsingCtxt.in - start <= pC->ReaderVideoAU.m_size)
                        {
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);
                            //WritebyteToMemory(code, in);
                            *in = code;
                            in++;
                        }
                    }
                    else
                    {
                        while (parsingCtxt.in - start < pC->ReaderVideoAU.m_size)
                        {
                            code = M4MCS_GetBitsFromMemory(&parsingCtxt, 8);
                            //WritebyteToMemory(code, in);
                            *in = code;
                            in++;
                        }
                    }
#endif
                    in--;

                    for(i=0;i<parsingCtxt.stream_index;i++)
                    {
                        stuffing_byte = stuffing_byte << 1;
                        stuffing_byte += 1;
                    }
                    M4MCS_WriteByteToMemory(stuffing_byte, (M4OSA_MemAddr32)in,
                        8-parsingCtxt.stream_index, parsingCtxt.stream_index);
                    pC->WriterVideoAU.size = in + 1 - pC->WriterVideoAU.dataAddress;
                    //*in ;
                }
            }
        }
    }

    return M4NO_ERROR;
}

#endif /* TIMESCALE_BUG */

