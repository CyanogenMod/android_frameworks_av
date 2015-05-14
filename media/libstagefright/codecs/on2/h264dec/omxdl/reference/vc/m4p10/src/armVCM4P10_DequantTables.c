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
 * File Name:  armVCM4P10_DequantTables.c
 * OpenMAX DL: v1.0.2
 * Revision:   9641
 * Date:       Thursday, February 7, 2008
 * 
 * 
 * 
 *
 * H.264 inverse quantize tables
 * 
 */
 
#include "omxtypes.h"
#include "armOMX.h"
#include "omxVC.h"
#include "armVC.h"


const OMX_U8 armVCM4P10_PosToVCol4x4[16] = 
{
    0, 2, 0, 2,
    2, 1, 2, 1,
    0, 2, 0, 2,
    2, 1, 2, 1
};

const OMX_U8 armVCM4P10_PosToVCol2x2[4] = 
{
    0, 2,
    2, 1
};

const OMX_U8 armVCM4P10_VMatrix[6][3] =
{
    { 10, 16, 13 },
    { 11, 18, 14 },
    { 13, 20, 16 },
    { 14, 23, 18 },
    { 16, 25, 20 },
    { 18, 29, 23 }
};
