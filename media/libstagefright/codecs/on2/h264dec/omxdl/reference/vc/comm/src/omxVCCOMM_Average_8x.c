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
/**
 * 
 * File Name:  omxVCCOMM_Average_8x.c
 * OpenMAX DL: v1.0.2
 * Revision:   9641
 * Date:       Thursday, February 7, 2008
 * 
 * 
 * 
 * Description:
 * This function will calculate Average of two 8x4 or 8x8 or 8x16 blocks
 * 
 */

#include "omxtypes.h"
#include "armOMX.h"
#include "omxVC.h"

#include "armCOMM.h"
#include "armVC.h"

/**
 * Function:  omxVCCOMM_Average_8x   (6.1.3.1.1)
 *
 * Description:
 * This function calculates the average of two 8x4, 8x8, or 8x16 blocks.  The 
 * result is rounded according to (a+b+1)/2.  The block average function can 
 * be used in conjunction with half-pixel interpolation to obtain quarter 
 * pixel motion estimates, as described in [ISO14496-10], subclause 8.4.2.2.1. 
 *
 * Input Arguments:
 *   
 *   pPred0     - Pointer to the top-left corner of reference block 0 
 *   pPred1     - Pointer to the top-left corner of reference block 1 
 *   iPredStep0 - Step of reference block 0 
 *   iPredStep1 - Step of reference block 1 
 *   iDstStep   - Step of the destination buffer. 
 *   iHeight    - Height of the blocks 
 *
 * Output Arguments:
 *   
 *   pDstPred - Pointer to the destination buffer. 8-byte aligned. 
 *
 * Return Value:
 *    
 *    OMX_Sts_NoErr - no error 
 *    OMX_Sts_BadArgErr - bad arguments; returned under any of the following 
 *              conditions: 
 *    -   one or more of the following pointers is NULL: pPred0, pPred1, or 
 *              pDstPred. 
 *    -   pDstPred is not aligned on an 8-byte boundary. 
 *    -   iPredStep0 <= 0 or iPredStep0 is not a multiple of 8. 
 *    -   iPredStep1 <= 0 or iPredStep1 is not a multiple of 8. 
 *    -   iDstStep   <= 0 or iDstStep is not a multiple of 8. 
 *    -   iHeight is not 4, 8, or 16. 
 *
 */
 OMXResult omxVCCOMM_Average_8x (	
	 const OMX_U8* 	    pPred0,
	 const OMX_U8* 	    pPred1,	
     OMX_U32		iPredStep0,
     OMX_U32		iPredStep1,
	 OMX_U8*		pDstPred,
     OMX_U32		iDstStep, 
	 OMX_U32		iHeight
)
{
    /* check for argument error */
    armRetArgErrIf(pPred0 == NULL, OMX_Sts_BadArgErr)
    armRetArgErrIf(pPred1 == NULL, OMX_Sts_BadArgErr)
    armRetArgErrIf(pDstPred == NULL, OMX_Sts_BadArgErr)
    armRetArgErrIf((iPredStep0 == 0) || (iPredStep0 & 7), OMX_Sts_BadArgErr)
    armRetArgErrIf((iPredStep1 == 0) || (iPredStep1 & 7), OMX_Sts_BadArgErr)
    armRetArgErrIf((iDstStep == 0) || (iDstStep & 7), OMX_Sts_BadArgErr)
    armRetArgErrIf((iHeight != 4) && (iHeight != 8) && (iHeight != 16), OMX_Sts_BadArgErr)
    armRetArgErrIf(armNot8ByteAligned(pDstPred), OMX_Sts_BadArgErr)

    return armVCCOMM_Average 
        (pPred0, pPred1, iPredStep0, iPredStep1, pDstPred, iDstStep, 8, iHeight);
}


/*****************************************************************************
 *                              END OF FILE
 *****************************************************************************/

