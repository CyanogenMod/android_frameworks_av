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
 ************************************************************************
 * @file         M4OSA_String.h
 * @ingroup      OSAL
 * @brief        public definition for string library
 ************************************************************************
*/

#ifndef _M4OSA_STRING_H_
#define _M4OSA_STRING_H_

#include "M4OSA_Types.h"
#include "M4OSA_FileCommon.h"
#include "M4OSA_Time.h"
#include "M4OSA_CharStar.h"



typedef void* M4OSA_String;

typedef enum
{
   M4OSA_kstrAll = 0,
   M4OSA_kstrBegin,
   M4OSA_kstrEnd
} M4OSA_strMode;

/* types definition */
typedef enum
{
   M4OSA_kstrDec   = M4OSA_kchrDec,
   M4OSA_kstrHexa  = M4OSA_kchrHexa,
   M4OSA_kstrOct   = M4OSA_kchrOct
} M4OSA_strNumBase;

/* Error and Warnings codes */
#define M4ERR_STR_BAD_STRING           M4OSA_ERR_CREATE(M4_ERR,M4OSA_STRING,0x000001)
#define M4ERR_STR_CONV_FAILED          M4OSA_ERR_CREATE(M4_ERR,M4OSA_STRING,0x000002)
#define M4ERR_STR_OVERFLOW             M4OSA_ERR_CREATE(M4_ERR,M4OSA_STRING,0x000003)
#define M4ERR_STR_BAD_ARGS             M4OSA_ERR_CREATE(M4_ERR,M4OSA_STRING,0x000004)

#define M4WAR_STR_OVERFLOW             M4OSA_ERR_CREATE(M4_WAR,M4OSA_STRING,0x000001)
#define M4WAR_STR_NOT_FOUND            M4OSA_ERR_CREATE(M4_WAR,M4OSA_STRING,0x000002)


#ifdef __cplusplus
extern "C"
{
#endif


M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strCreate(                M4OSA_String* pstr);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strReset(                 M4OSA_String str);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strDestroy(               M4OSA_String str);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetCharContent(        M4OSA_String str,
                                          M4OSA_Char* pchar);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetCharContent(        M4OSA_String str,
                                          M4OSA_Char** ppchar);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetChar(               M4OSA_String str,
                                          M4OSA_Char c);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetChar(               M4OSA_String str,
                                          M4OSA_Char* pc);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetInt8(               M4OSA_String str,
                                          M4OSA_Int8 i8,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetInt8(               M4OSA_String str,
                                          M4OSA_Int8* pi8,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetUInt8(              M4OSA_String str,
                                          M4OSA_UInt8 ui8,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetUInt8(              M4OSA_String str,
                                          M4OSA_UInt8* pui8,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetInt16(              M4OSA_String str,
                                          M4OSA_Int16 i16,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetInt16(              M4OSA_String str,
                                          M4OSA_Int16* pi16,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetUInt16(             M4OSA_String str,
                                          M4OSA_UInt16 ui16,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetUInt16(             M4OSA_String str,
                                          M4OSA_UInt16* pui16,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetInt32(              M4OSA_String str,
                                          M4OSA_Int32 i32,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetInt32(              M4OSA_String str,
                                          M4OSA_Int32* pi32,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetUInt32(             M4OSA_String str,
                                          M4OSA_UInt32 ui32,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetUInt32(             M4OSA_String str,
                                          M4OSA_UInt32* pui32,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetDouble(             M4OSA_String str,
                                          M4OSA_Double d);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetDouble(             M4OSA_String str,
                                          M4OSA_Double* pd);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetInt64(              M4OSA_String str,
                                          M4OSA_Int64 i64,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetInt64(              M4OSA_String str,
                                          M4OSA_Int64* pi64,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetFilePosition(       M4OSA_String str,
                                          M4OSA_FilePosition fpos,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetFilePosition(       M4OSA_String str,
                                          M4OSA_FilePosition* pfpos,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetTime(               M4OSA_String str,
                                          M4OSA_Time t,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetTime(               M4OSA_String str,
                                          M4OSA_Time* pt,
                                          M4OSA_strNumBase base);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetLength(             M4OSA_String str,
                                          M4OSA_UInt32 *pui32);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strTruncate(              M4OSA_String str,
                                          M4OSA_UInt32 ui32_length);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strCopy(                  M4OSA_String str_in,
                                          M4OSA_String str_out);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strCopySubStr(            M4OSA_String str_out,
                                          M4OSA_UInt32 ui32_pos,
                                          M4OSA_String str_in,
                                          M4OSA_UInt32 ui32_offset,
                                          M4OSA_UInt32* ui32_num);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strConcat(                M4OSA_String str_first,
                                          M4OSA_String str_second);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strInsertSubStr(          M4OSA_String str_out,
                                          M4OSA_UInt32 ui32_pos,
                                          M4OSA_String str_in,
                                          M4OSA_UInt32 ui32_offset,
                                          M4OSA_UInt32* pui32_num);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strCompare(               M4OSA_String str_in1,
                                          M4OSA_String str_in2,
                                          M4OSA_Int32* pi32_result);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strCompareSubStr(         M4OSA_String str_in1,
                                          M4OSA_UInt32 ui32_offset1,
                                          M4OSA_String str_in2,
                                          M4OSA_UInt32 ui32_offset2,
                                          M4OSA_UInt32* pui32_num,
                                          M4OSA_Int32* pi32_result);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strCaseCompare(           M4OSA_String str_in1,
                                          M4OSA_String str_in2,
                                          M4OSA_Int32* pi32_result);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strCaseCompareSubStr(     M4OSA_String str_in1,
                                          M4OSA_UInt32 ui32_offset1,
                                          M4OSA_String str_in2,
                                          M4OSA_UInt32 ui32_offset2,
                                          M4OSA_UInt32* pui32_num,
                                          M4OSA_Int32* pi32_result);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSpan(                  M4OSA_String str_in,
                                          M4OSA_Char* charset,
                                          M4OSA_UInt32* pui32_result);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSpanComplement(        M4OSA_String str_in,
                                          M4OSA_Char* charset,
                                          M4OSA_UInt32* pui32_pos);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strFindFirstChar(         M4OSA_String str_in,
                                          M4OSA_Char c,
                                          M4OSA_UInt32* pui32_pos);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strFindLastChar(          M4OSA_String str_in,
                                          M4OSA_Char c,
                                          M4OSA_UInt32* pui32_pos);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strFindFirstSubStr(       M4OSA_String str_in1,
                                          M4OSA_String str_in2,
                                          M4OSA_UInt32* pui32_pos);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strFindLastSubStr(        M4OSA_String str_in1,
                                          M4OSA_String str_in2,
                                          M4OSA_UInt32* pui32_pos);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetFirstToken(         M4OSA_String str_in,
                                          M4OSA_String str_delim,
                                          M4OSA_String pstr_token);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strGetLastToken(          M4OSA_String str_in,
                                          M4OSA_String str_delim,
                                          M4OSA_String pstr_token);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetUpperCase(          M4OSA_String str);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetLowerCase(          M4OSA_String str);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strDelSubStr(             M4OSA_String str_in,
                                          M4OSA_UInt32 ui32_offset,
                                          M4OSA_UInt32* ui32_num);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strReplaceSubStr(         M4OSA_String str_in,
                                          M4OSA_String str_old,
                                          M4OSA_String str_new,
                                          M4OSA_strMode mode);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSprintf(               M4OSA_String str,
                                          M4OSA_Char* format,
                                          ...);

M4OSAL_STRING_EXPORT_TYPE M4OSA_ERR M4OSA_strSetMinAllocationSize(  M4OSA_String str,
                                          M4OSA_UInt32 ui32_size);

#ifdef __cplusplus
}
#endif

#endif

