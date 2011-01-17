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

#ifndef __M4OSA_FILEACESS_H__
#define __M4OSA_FILEACESS_H__

#include "M4OSA_Types.h"
#include "M4OSA_Semaphore.h"
#include "M4OSA_Debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 ******************************************************************************
 * struct        M4OSA_FilePtrFct
 * @brief        Defines the available functions for File read/write.
 ******************************************************************************
*/
typedef enum
{
    M4OSA_FILEINTERFACE_RAM,
    M4OSA_FILEINTERFACE_FFS,
    M4OSA_FILEINTERFACE_OPTIM_FFS

} M4OSA_FileInterface_t;


typedef struct
{
    M4OSA_FileWriterPointer *pFileWriter;    /**< Pointer to file writer functions */
    M4OSA_FileReadPointer   *pFileReader;    /**< Pointer to file reader functions */
} M4OSA_FilePtrFct;

/*Semaphore for handling of R/W access*/
extern M4OSA_Context    M4OSA_FileInterface_RWsemaphore; /*defined in M4OSA_FileInterface.c*/

/**
 ******************************************************************************
 * func         M4_FileInterface_xxx
 * @brief        Manage the interface pointers for filesystem access
 ******************************************************************************
*/
M4OSA_FilePtrFct* M4_FileInterface_InitPointer(void);
M4OSA_ERR M4_FileInterface_SelectPointer(M4OSA_FilePtrFct *pFileFctPtr,
                                         M4OSA_FileInterface_t mode);
M4OSA_Void M4_FileInterface_FreePointer(M4OSA_FilePtrFct *pFileFctPtr);
M4OSA_ERR M4OSA_fileReadOpen_optim_SetInterfaceFFS(M4OSA_Context* pContext,
                                                   M4OSA_Void* pFileDescriptor,
                                                   M4OSA_UInt32 FileModeAccess);

#ifdef __cplusplus
}
#endif /* __cplusplus*/

#endif /* __M4OSA_FILEACESS_H__*/
