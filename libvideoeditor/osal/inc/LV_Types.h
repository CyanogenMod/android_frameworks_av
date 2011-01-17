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

/*******************************************************************************
* @file        LV_Types.h
* @brief    Types definition for Smartphone team
*******************************************************************************/

#ifndef LV_TYPES_H
#define LV_TYPES_H

/*------------*/
/*    INCLUDES  */
/*------------*/
#include "M4OSA_Error.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*--------------------------------------*/
/*    CHARACTER ENCODING CONVERSION FCTS    */
/*--------------------------------------*/
/******************************************************************************
*
* M4OSA_UInt32 (*LV_fromUTF8ToNative_Fct)(const M4OSA_Char* pStart,
*                                          M4OSA_Void** pOut);
* @note        This function converts a string from UTF8 format which is the default
*            encoding in the engines and application logics to the character encoding
*            supported by the OS or platform. The memory will be allocated within this
*            function and then, caller will have to free *targetStart thanks to M4OSA_free.
*            Both strings must be NULL-terminateed.
* @param    pStart        (IN):    String to convert.
* @param    pOut        (OUT):    This pointer will be filled by this function. It contains the
*                                string converted to the native format.
* @return    Success: Size in bytes allocated including the NULL character. Failure: 0
*
******************************************************************************/
typedef M4OSA_UInt32 (*LV_fromUTF8ToNative_Fct)(const M4OSA_Char* pStart,
                                                M4OSA_Void** pOut);

/******************************************************************************
*
* M4OSA_UInt32 (*LV_fromNativeToUTF8_Fct)(const M4OSA_Char* pStart,
*                                          M4OSA_Void** targetStart);
* @note        This function converts a string in the character encoding supported by the
*            OS or platform to from UTF8 format which is the default encoding in the
*            engines and application logics. The memory will be allocated within this
*            function and then, caller will have to free *targetStart thanks to M4OSA_free.
*            Both strings must be NULL-terminateed.
* @param    pStart        (IN):    String to convert.
* @param    pOut        (OUT):    This pointer will be filled by this function. It contains the
*                                string converted to UTF8 format.
* @return    Success: Size in bytes allocated including the NULL character. Failure: 0
*
******************************************************************************/
typedef M4OSA_UInt32 (*LV_fromNativeToUTF8_Fct)(const M4OSA_Void** pStart,
                                                M4OSA_Char** pOut);
#ifdef __cplusplus
}
#endif

#endif /*---  LV_TYPES_H ---*/
