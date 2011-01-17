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


#ifndef _M4OSA_STRING_PRIV_H
#define _M4OSA_STRING_PRIV_H


#include "M4OSA_Types.h"
#include "M4OSA_String.h"

#include <stdarg.h>

typedef struct
{
   /* string identifiant */
   M4OSA_UInt32   coreID;
   /** data buffer */
   M4OSA_Char* pui8_buffer;
   /** allocated size of the data buffer */
   M4OSA_UInt32 ui32_size;
   /** size of valid data in the buffer */
   M4OSA_UInt32 ui32_length;
} M4OSA_strStruct;



M4OSA_ERR M4OSA_strPrivRealloc(              M4OSA_strStruct* str,
                                             M4OSA_UInt32 ui32_length);

M4OSA_ERR M4OSA_strPrivReallocCopy(          M4OSA_strStruct* str,
                                             M4OSA_UInt32 ui32_length);

M4OSA_ERR M4OSA_strPrivSet(                  M4OSA_strStruct* str,
                                             M4OSA_Char* pchar,
                                             M4OSA_UInt32 ui32_length);

M4OSA_ERR M4OSA_strPrivDuplicate(            M4OSA_strStruct** ostr,
                                             M4OSA_strStruct* istr);

M4OSA_Int32 M4OSA_strPrivFindLastSubStr(     M4OSA_strStruct* str1,
                                             M4OSA_strStruct* str2,
                                             M4OSA_UInt32 ui32_pos);

M4OSA_ERR M4OSA_strPrivSetAndRepleceStr(     M4OSA_strStruct* istr,
                                             M4OSA_UInt32 ui32_pos,
                                             M4OSA_UInt32 olength,
                                             M4OSA_Char* nbuff,
                                             M4OSA_UInt32 nlength);

M4OSA_ERR M4OSA_strPrivReplaceSameSizeStr(   M4OSA_strStruct* istr,
                                             M4OSA_strStruct* ostr,
                                             M4OSA_strStruct* nstr,
                                             M4OSA_strMode mode);

M4OSA_ERR M4OSA_strPrivReplaceSmallerStr(    M4OSA_strStruct* istr,
                                             M4OSA_strStruct* ostr,
                                             M4OSA_strStruct* nstr,
                                             M4OSA_strMode mode);

M4OSA_ERR M4OSA_strPrivReplaceBiggerStr(     M4OSA_strStruct* istr,
                                             M4OSA_strStruct* ostr,
                                             M4OSA_strStruct* nstr,
                                             M4OSA_strMode mode);

M4OSA_ERR M4OSA_strPrivSPrintf(              M4OSA_strStruct* str,
                                             M4OSA_Char *format,
                                             va_list marker);


#define M4OSA_CHECK_MALLOC(buff, string)\
   if(buff == M4OSA_NULL)\
   {\
      M4OSA_DEBUG(M4ERR_ALLOC, string);\
      return M4ERR_ALLOC;\
   }\


#endif

