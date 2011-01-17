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
 * @file         M4OSA_FileReaderRam.h
 * @ingroup      OSAL
 * @note         This file implements functions to read a "file" stored in RAM.
 ******************************************************************************
*/



#ifndef M4OSA_FILEREADERRAM_H
#define M4OSA_FILEREADERRAM_H

#include "M4OSA_FileReader.h"

/**
 ******************************************************************************
 * structure    M4FI_FileReaderRam_Descriptor
 * @brief        This structure defines the File descriptor (public)
 * @note        This structure is used to store the pointer to the data in memory
 * @note        and its size
 ******************************************************************************
*/
typedef struct
{
    M4OSA_MemAddr8    pFileDesc;    /* Pointer on file data */
    M4OSA_Int32        dataSize;    /* Size of data */
} M4OSA_FileReaderRam_Descriptor;

#ifdef __cplusplus
extern "C"
{
#endif

/* reader API : RAM functions */
M4OSA_ERR M4OSA_fileReadRamOpen( M4OSA_Context* context,
                                 M4OSA_Void* fileDescriptor,
                                 M4OSA_UInt32 fileModeAccess );
M4OSA_ERR M4OSA_fileReadRamData( M4OSA_Context context,
                                 M4OSA_MemAddr8 buffer,
                                 M4OSA_UInt32* size );
M4OSA_ERR M4OSA_fileReadRamSeek( M4OSA_Context context,
                                 M4OSA_FileSeekAccessMode seekMode,
                                 M4OSA_FilePosition* position );
M4OSA_ERR M4OSA_fileReadRamClose( M4OSA_Context context );
M4OSA_ERR M4OSA_fileReadRamGetOption( M4OSA_Context context,
                                      M4OSA_FileReadOptionID optionID,
                                      M4OSA_DataOption *optionValue );
M4OSA_ERR M4OSA_fileReadRamSetOption( M4OSA_Context context,
                                      M4OSA_FileReadOptionID optionID,
                                      M4OSA_DataOption optionValue );


#ifdef __cplusplus
}
#endif

#endif   /*M4OSA_FILEREADERRAM_H*/

