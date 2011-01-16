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
 * @file        M4TRAN_transition.h
 * @brief
 * @note
 ******************************************************************************
*/

#ifndef __M4VFL_TRANSITION_H__
#define __M4VFL_TRANSITION_H__

#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned char UInt8;
typedef unsigned long UInt32;

typedef    struct S_M4ViComImagePlane
{
    UInt32        u_width;            /* active width, in pixels */
    UInt32        u_height;            /* active height, in lines */
    UInt32        u_topleft;            /* index of 1st active pixel */
    UInt32        u_stride;            /* line stride, in bytes */
    UInt8        *pac_data;            /* buffer address */
}    M4ViComImagePlane;

typedef struct S_M4VFL_modifLumParam
{
    unsigned short lum_factor;
    unsigned short copy_chroma;
} M4VFL_ModifLumParam;

typedef struct S_M4VFL_CurtainParam
{
    unsigned short nb_black_lines;
    unsigned char top_is_black;
} M4VFL_CurtainParam;

#define     M4VIFI_OK                       0
#define     M4VIFI_ILLEGAL_FRAME_HEIGHT     8
#define     M4VIFI_ILLEGAL_FRAME_WIDTH      9

unsigned char M4VFL_modifyLumaByStep(M4ViComImagePlane *plane_in, M4ViComImagePlane *plane_out,
                                         M4VFL_ModifLumParam *lum_param, void *user_data);

unsigned char M4VFL_modifyLumaWithScale(M4ViComImagePlane *plane_in, M4ViComImagePlane *plane_out,
                                         unsigned long lum_factor, void *user_data);

unsigned char M4VFL_applyClosingCurtain(M4ViComImagePlane *plane_in, M4ViComImagePlane *plane_out,
                                         unsigned short curtain_factor, void *user_data);

unsigned char M4VFL_applyOpeningCurtain(M4ViComImagePlane *plane_in, M4ViComImagePlane *plane_out,
                                         unsigned short curtain_factor, void *user_data);

unsigned char M4VFL_applyFallingCurtain(M4ViComImagePlane *plane_in, M4ViComImagePlane *plane_out,
                                         unsigned short curtain_factor, void *user_data);


/**
 ******************************************************************************
 * unsigned char M4VFL_applyCurtain(M4ViComImagePlane *plane_in, M4ViComImagePlane *plane_out,
 *                                   M4VFL_CurtainParam *curtain_factor, void *user_data)
 * @brief    This function applies a black curtain onto a YUV420 image.
 * @note    THis function writes black lines either at the top of the image or at
 *            the bottom of the image. The other lines are copied from the source image.
 *            First the number of black lines is compted and is rounded to an even integer.
 * @param    plane_in: (IN) pointer to the 3 image planes of the source image
 * @param    plane_out: (OUT) pointer to the 3 image planes of the destination image
 * @param    user_data: (IN) pointer to some user_data
 * @param    curtain_factor: (IN) structure with the parameters of the curtain (nb of black lines
 *                                and if at the top/bottom of the image)
 * @return    0: there is no error
 ******************************************************************************
*/
unsigned char M4VFL_applyCurtain(M4ViComImagePlane *plane_in, M4ViComImagePlane *plane_out,
                                    M4VFL_CurtainParam *curtain_factor, void *user_data);


/**
 *************************************************************************************************
 * M4OSA_ERR M4VIFI_ImageBlendingonYUV420 (void *pUserData,
 *                                                  M4VIFI_ImagePlane *pPlaneIn1,
 *                                                  M4VIFI_ImagePlane *pPlaneIn2,
 *                                                  M4VIFI_ImagePlane *pPlaneOut,
 *                                                  M4VIFI_UInt32 Progress)
 * @brief   Blends two YUV 4:2:0 Planar images.
 * @note    Blends YUV420 planar images,
 *          Map the value of progress from (0 - 1000) to (0 - 1024)
 *          Set the range of blendingfactor,
 *                  1. from 0 to (Progress << 1)            ;for Progress <= 512
 *                  2. from (( Progress - 512)<< 1) to 1024 ;otherwise
 *          Set the increment of blendingfactor for each element in the image row by the factor,
 *                  =  (Range-1) / (image width-1)  ;for width >= range
 *                  =  (Range) / (image width)      ;otherwise
 *          Loop on each(= i) row of output Y plane (steps of 2)
 *              Loop on each(= j) column of output Y plane (steps of 2)
 *                  Get four Y samples and one U & V sample from two input YUV4:2:0 images and
 *                  Compute four Y sample and one U & V sample for output YUV4:2:0 image
 *                      using the following,
 *                  Out(i,j) = blendingfactor(i,j) * In1(i,j)+ (l - blendingfactor(i,j)) * In2(i,j)
 *              end loop column
 *          end loop row.
 * @param   pUserData: (IN)  User Specific Parameter
 * @param   pPlaneIn1: (IN)  Pointer to an array of image plane structures maintained for Y, U
 *                            and V planes.
 * @param   pPlaneIn2: (IN)  Pointer to an array of image plane structures maintained for Y, U
 *                            and V planes.
 * @param   pPlaneOut: (OUT) Pointer to an array of image plane structures maintained for Y, U
 *                            and V planes.
 * @param   Progress:  (IN)  Progress value (varies between 0 and 1000)
 * @return  M4VIFI_OK: No error
 * @return  M4VIFI_ILLEGAL_FRAME_HEIGHT: Error in height
 * @return  M4VIFI_ILLEGAL_FRAME_WIDTH:  Error in width
 ***********************************************************************************************/
unsigned char M4VIFI_ImageBlendingonYUV420 (void *pUserData, M4ViComImagePlane *pPlaneIn1,
                                                M4ViComImagePlane *pPlaneIn2,
                                                M4ViComImagePlane *pPlaneOut, UInt32 Progress);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif // __M4VFL_TRANSITION_H__
