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
 * @file         M4OSA_FileCache.h
 * @ingroup      OSAL
 * @brief         Osal File Reader and Writer with cache
 * @note          This file implements functions to manipulate
 *                 filesystem access with intermediate buffers used to
 *                  read and to write.
 ******************************************************************************
*/

#ifndef M4OSA_FILECACHE_H
#define M4OSA_FILECACHE_H

#ifdef __cplusplus
extern "C"
{
#endif

M4OSA_ERR M4OSA_fileOpen_cache(M4OSA_Context* pContext,
                               M4OSA_Void* pFileDescriptor,
                               M4OSA_UInt32 FileModeAccess);
M4OSA_ERR M4OSA_fileReadData_cache( M4OSA_Context context,
                                    M4OSA_MemAddr8 buffer,
                                    M4OSA_UInt32* size );
M4OSA_ERR M4OSA_fileWriteData_cache( M4OSA_Context context,
                                     M4OSA_MemAddr8 buffer,
                                     M4OSA_UInt32 size );
M4OSA_ERR M4OSA_fileReadSeek_cache( M4OSA_Context context,
                                    M4OSA_FileSeekAccessMode seekMode,
                                    M4OSA_FilePosition* position );
M4OSA_ERR M4OSA_fileWriteSeek_cache( M4OSA_Context context,
                                     M4OSA_FileSeekAccessMode seekMode,
                                     M4OSA_FilePosition* position );
M4OSA_ERR M4OSA_fileGetOption_cache( M4OSA_Context context,
                                     M4OSA_OptionID optionID,
                                     M4OSA_DataOption *optionValue );
M4OSA_ERR M4OSA_fileSetOption_cache( M4OSA_Context context,
                                     M4OSA_OptionID optionID,
                                     M4OSA_DataOption optionValue );
M4OSA_ERR M4OSA_fileFlush_cache( M4OSA_Context pContext);
M4OSA_ERR M4OSA_fileClose_cache( M4OSA_Context context );

/* Used in VA */
M4OSA_ERR M4OSA_fileExtrafTruncate_cache(M4OSA_Context context,
                                         M4OSA_UInt32 length);

#ifdef __cplusplus
}
#endif


#endif /* M4OSA_FILECACHE_H */
