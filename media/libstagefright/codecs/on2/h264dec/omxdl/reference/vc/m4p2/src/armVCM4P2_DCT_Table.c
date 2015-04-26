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
 * File Name:  armVCM4P2_DCT_Table.c
 * OpenMAX DL: v1.0.2
 * Revision:   9641
 * Date:       Thursday, February 7, 2008
 * 
 * 
 * 
 *
 * File:        armVCM4P2_DCT_Table.c
 * Description: Contains the DCT/IDCT coefficent matrix
 *
 */

#ifndef _OMXDCTCOSTAB_C_
#define _OMXDCTCOSTAB_C_

#include "omxtypes.h"
#include "armOMX.h"

const OMX_F64 armVCM4P2_preCalcDCTCos[8][8] =
{
        {
                0.353553390593273730, 
                0.490392640201615220, 
                0.461939766255643370, 
                0.415734806151272620, 
                0.353553390593273790, 
                0.277785116509801140, 
                0.191341716182544920, 
                0.097545161008064152 
        },
        {
                0.353553390593273730, 
                0.415734806151272620, 
                0.191341716182544920, 
                -0.097545161008064096, 
                -0.353553390593273730, 
                -0.490392640201615220, 
                -0.461939766255643420, 
                -0.277785116509801090
        },
        {
                0.353553390593273730, 
                0.277785116509801140, 
                -0.191341716182544860, 
                -0.490392640201615220, 
                -0.353553390593273840, 
                0.097545161008064138, 
                0.461939766255643260, 
                0.415734806151272730 
        },
        {
                0.353553390593273730, 
                0.097545161008064152, 
                -0.461939766255643370, 
                -0.277785116509801090, 
                0.353553390593273680, 
                0.415734806151272730, 
                -0.191341716182544920, 
                -0.490392640201615330
        },
        {
                0.353553390593273730, 
                -0.097545161008064096, 
                -0.461939766255643420, 
                0.277785116509800920, 
                0.353553390593273840, 
                -0.415734806151272620, 
                -0.191341716182545280, 
                0.490392640201615220 
        },
        {
                0.353553390593273730, 
                -0.277785116509800980, 
                -0.191341716182545170, 
                0.490392640201615220, 
                -0.353553390593273340, 
                -0.097545161008064013, 
                0.461939766255643370, 
                -0.415734806151272510
        },
        {
                0.353553390593273730, 
                -0.415734806151272670, 
                0.191341716182545000, 
                0.097545161008064388, 
                -0.353553390593273620, 
                0.490392640201615330, 
                -0.461939766255643200, 
                0.277785116509800760 
        },
        {
                0.353553390593273730, 
                -0.490392640201615220, 
                0.461939766255643260, 
                -0.415734806151272620, 
                0.353553390593273290, 
                -0.277785116509800760, 
                0.191341716182544780, 
                -0.097545161008064277
        }
};

#endif /*_OMXDCTCOSTAB_C_*/


/* End of file */


