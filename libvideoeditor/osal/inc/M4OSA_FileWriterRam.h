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
 * @file         M4OSA_FileWriterRam.h
 * @ingroup      OSAL
 * @brief        File writer to RAM
 * @note         This file implements functions to write a "file" in RAM.
 ******************************************************************************
*/


#ifndef M4OSA_FILEWRITERRAM_H
#define M4OSA_FILEWRITERRAM_H

#include "M4OSA_FileWriter.h"


/**
 ******************************************************************************
 * structure    M4OSA_FileWriterRam_Descriptor
 * @brief        This structure defines the File descriptor (public)
 * @note        This structure is used to store the pointer to the data in memory
 * @note        and its size
 ******************************************************************************
*/
typedef struct
{
    M4OSA_MemAddr8    pFileDesc;    /* Pointer on file data */
    M4OSA_Int32        dataSize;    /* Size of data */
} M4OSA_FileWriterRam_Descriptor;

#ifdef __cplusplus
extern "C"
{
#endif

/* Writer API : RAM functions */
M4OSA_ERR M4OSA_fileWriteRamOpen(M4OSA_Context* context,
                                 M4OSA_Void* fileDescriptor,
                                 M4OSA_UInt32 fileModeAccess);
M4OSA_ERR M4OSA_fileWriteRamData(M4OSA_Context context,
                                 M4OSA_MemAddr8 data,
                                 M4OSA_UInt32 Size);
M4OSA_ERR M4OSA_fileWriteRamSeek(M4OSA_Context context,
                                 M4OSA_FileSeekAccessMode SeekMode,
                                 M4OSA_FilePosition* position);
M4OSA_ERR M4OSA_fileWriteRamClose(M4OSA_Context context);
M4OSA_ERR M4OSA_fileWriteRamFlush(M4OSA_Context context);
M4OSA_ERR M4OSA_fileWriteRamSetOption(M4OSA_Context context,
                                      M4OSA_OptionID OptionID,
                                      M4OSA_DataOption OptionValue);
M4OSA_ERR M4OSA_fileWriteRamGetOption(M4OSA_Context context,
                                      M4OSA_OptionID OptionID,
                                      M4OSA_DataOption* optionValue);


#ifdef __cplusplus
}
#endif

#endif   /*M4OSA_FILEWRITERRAM_H*/

