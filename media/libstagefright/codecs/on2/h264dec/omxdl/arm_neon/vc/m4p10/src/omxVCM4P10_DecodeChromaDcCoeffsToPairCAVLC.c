/*
 * Copyright (C) 2007-2008 ARM Limited
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
 *
 */
/* ----------------------------------------------------------------
 *
 * 
 * File Name:  omxVCM4P10_DecodeChromaDcCoeffsToPairCAVLC.c
 * OpenMAX DL: v1.0.2
 * Revision:   12290
 * Date:       Wednesday, April 9, 2008
 * 
 * 
 * 
 *
 * H.264 decode coefficients module
 * 
 */
 
#include "omxtypes.h"
#include "armOMX.h"
#include "omxVC.h"

#include "armCOMM.h"
#include "armVC.h"

/**
 * Function: omxVCM4P10_DecodeChromaDcCoeffsToPairCAVLC
 *
 * Description:
 * Performs CAVLC decoding and inverse raster scan for 2x2 block of 
 * ChromaDCLevel. The decoded coefficients in packed position-coefficient 
 * buffer are stored in increasing raster scan order, namely position order.
 *
 * Remarks:
 *
 * Parameters:
 * [in]	ppBitStream		Double pointer to current byte in bit stream
 *								buffer
 * [in]	pOffset			Pointer to current bit position in the byte 
 *								pointed to by *ppBitStream
 * [out]	ppBitStream		*ppBitStream is updated after each block is decoded
 * [out]	pOffset			*pOffset is updated after each block is decoded
 * [out]	pNumCoeff		Pointer to the number of nonzero coefficients
 *								in this block
 * [out]	ppPosCoefbuf	Double pointer to destination residual
 *								coefficient-position pair buffer
 *
 * Return Value:
 * Standard omxError result. See enumeration for possible result codes.
 *
 */

OMXResult omxVCM4P10_DecodeChromaDcCoeffsToPairCAVLC (
     const OMX_U8** ppBitStream,
     OMX_S32* pOffset,
     OMX_U8* pNumCoeff,
     OMX_U8** ppPosCoefbuf        
 )

{
    return armVCM4P10_DecodeCoeffsToPair(ppBitStream, pOffset, pNumCoeff,
                                         ppPosCoefbuf, 17, 4);

}
