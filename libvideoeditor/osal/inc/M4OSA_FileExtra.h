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
/************************************************************************/
/*                                                                      */
/* @file        M4OSA_FileExtra.h                                        */
/* @ingroup     OSAL                                                    */
/************************************************************************/


#ifndef M4OSA_FILE_EXTRA_H
#define M4OSA_FILE_EXTRA_H

#include "M4OSA_Types.h"
#include "M4OSA_Error.h"
#include "M4OSA_FileCommon.h"
/* size of the copy buffer (in bytes) for M4OSA_fileExtraCopy */
#define BUFFER_COPY_SIZE 1024

typedef enum
{
    M4OSA_TypeInvalid = 0,
    M4OSA_TypeFile,
    M4OSA_TypeDir
} M4OSA_EntryType;

#ifdef __cplusplus
extern "C"
{
#endif

M4OSA_ERR M4OSA_fileExtraDelete(const M4OSA_Char* url);

M4OSA_ERR M4OSA_fileExtraCopy(M4OSA_Char* srcUrl, M4OSA_Char* dstUrl);

M4OSA_ERR M4OSA_fileExtraRename(M4OSA_Char* srcUrl, M4OSA_Char* dstUrl);

M4OSA_ERR M4OSA_fileExtraChangeCurrentDir(const M4OSA_Char* url);

M4OSA_ERR M4OSA_fileExtraCreateDir(const M4OSA_Char* pUrl);

M4OSA_ERR M4OSA_fileExtraRemoveDir(const M4OSA_Char* pUrl);

M4OSA_UInt32 M4OSA_fileExtraGetFreeSpace(const M4OSA_Char* pUrl);

M4OSA_UInt32 M4OSA_fileExtraGetTotalSpace(const M4OSA_Char* pUrl);

M4OSA_EntryType M4OSA_fileExtraGetType(const M4OSA_Char* pUrl);

M4OSA_ERR M4OSA_fileExtrafTruncate(M4OSA_Context context, M4OSA_FilePosition length);

#ifdef __cplusplus
}
#endif

#endif   /*M4OSA_FILE_EXTRA_H*/

