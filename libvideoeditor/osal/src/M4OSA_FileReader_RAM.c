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
 * @file         M4OSA_FileReaderRam.c
 * @ingroup      OSAL
 * @brief        File reader from RAM
 * @note         This file implements functions to read a "file" stored in RAM.
 * @date         - 2004-05-11: creation
 ******************************************************************************
*/

#include "M4OSA_Debug.h"
#include "M4OSA_FileReaderRam.h"
#include "M4OSA_Memory.h"

/**
 ******************************************************************************
 * structure    M4OSA_FileReaderRam_Context
 * @brief       This structure defines the File reader in Ram context (private)
 * @note        This structure is used for all File Reader calls to store the context
 ******************************************************************************
*/
typedef struct
{
    M4OSA_MemAddr8  pFileDesc;  /* Pointer on file data */
    M4OSA_UInt32    dataSize;   /* Size of data to read */
    M4OSA_UInt32    dataOffset; /* Actual offset */
    M4OSA_Bool      IsOpened;   /* Micro state machine */
} M4OSA_FileReaderRam_Context;

/**
 ******************************************************************************
 * @brief      This function sets the read pointer at the provided adress and
 *             returns a context.
 *             If an error occured, the context is set to NULL.
 * @param      context: (OUT) Context of the core file reader
 * @param      url: (IN) URL of the input file
 * @param      fileModeAccess: (IN) File mode access
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4ERR_NOT_IMPLEMENTED: the URL does not match with the supported
 *             file
 * @return     M4ERR_FILE_NOT_FOUND: the file cannot be found
 * @return     M4ERR_FILE_LOCKED: the file is locked by an other
 *             application/process
 * @return     M4ERR_FILE_BAD_MODE_ACCESS: the file mode access is not correct
 ******************************************************************************
*/
M4OSA_ERR M4OSA_fileReadRamOpen( M4OSA_Context* context,
                                 M4OSA_Void* fileDescriptor,
                                 M4OSA_UInt32 fileModeAccess )
{
    M4OSA_FileReaderRam_Context* pContext=M4OSA_NULL;
    M4OSA_FileReaderRam_Descriptor* pDescriptor=fileDescriptor;
    M4OSA_ERR err = M4NO_ERROR;

    M4OSA_TRACE3_3("M4OSA_fileReadRamOpen\t\tM4OSA_Context* 0x%x\tM4OSA_Void* 0x%x"
                  "\tM4OSA_UInt32 %d", context, fileDescriptor,
                  fileModeAccess);

    /* Check input parameters */
    if(M4OSA_NULL == context)
    {
        return M4ERR_PARAMETER;
    }
    *context = M4OSA_NULL;
    if(M4OSA_NULL == fileDescriptor)
    {
        return M4ERR_PARAMETER;
    }

    /* Allocates memory for the context */
    pContext = (M4OSA_FileReaderRam_Context*)M4OSA_32bitAlignedMalloc(sizeof(M4OSA_FileReaderRam_Context),
                          M4OSA_FILE_READER, (M4OSA_Char*)"Context allocation");
    if(pContext == M4OSA_NULL)
    {
        return M4ERR_ALLOC;
    }

    /* Verify access mode */
    if (((fileModeAccess & M4OSA_kFileAppend) != 0)
       ||(0 == (fileModeAccess & M4OSA_kFileRead)))
    {
        err=M4ERR_FILE_BAD_MODE_ACCESS;
        goto cleanup;
    }

    /* Open file in read mode and in binary/text mode  with/without creation right */
    if((fileModeAccess & M4OSA_kFileCreate) != 0)
    {
        err=M4ERR_FILE_BAD_MODE_ACCESS;
    }
    else
    {
        if ((fileModeAccess & M4OSA_kFileRead))
        {
            pContext->pFileDesc = (M4OSA_MemAddr8)(pDescriptor->pFileDesc);
            pContext->dataSize = (M4OSA_Int32)(pDescriptor->dataSize);
            pContext->dataOffset = 0;
            pContext->IsOpened = M4OSA_TRUE;
        }
        else
        {
            err=M4ERR_FILE_BAD_MODE_ACCESS;
        }
    }

cleanup:
    if(err != M4NO_ERROR)
    {
        if(pContext != M4OSA_NULL)
        {
            free(pContext);
            *context = M4OSA_NULL;
        }
    }
    else
    {
        *context = pContext;
    }
    return err;
}

/**
 ******************************************************************************
  * @brief      This function reads the 'size' bytes in memory
 *             (selected by its 'context') and writes the data to the 'data'
 *             pointer.
 * @note       If 'size' byte cannot be read in the core file reader, 'size'
 *             parameter is updated to match the correct
 *             number of read bytes.
 * @param      context: (IN/OUT) Context of the core file reader
 * @param      data: (OUT) Data pointer of the read data
 * @param      size: (IN/OUT) Size of the data to read (in bytes)
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_BAD_CONTEXT: provided context is not a valid one
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4WAR_NO_DATA_YET: there is no enough data to fill the 'data'
 *             buffer, so the size parameter has been updated.
 ******************************************************************************
*/
M4OSA_ERR M4OSA_fileReadRamData( M4OSA_Context context, M4OSA_MemAddr8 data,
                                                           M4OSA_UInt32* pSize )
{
    M4OSA_FileReaderRam_Context* pContext=(M4OSA_FileReaderRam_Context*)context;
    M4OSA_UInt32 aSize = *pSize;
    M4OSA_ERR err = M4NO_ERROR;

    /* Check input parameters */
    if(context == M4OSA_NULL || data == M4OSA_NULL || pSize == M4OSA_NULL)
    {
        return M4ERR_PARAMETER;
    }
    if (pContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;
    }

    /* Check if there is enough data to read or not */
    if((pContext->dataOffset + aSize) > pContext->dataSize)
    {
        aSize = pContext->dataSize - pContext->dataOffset;
        M4OSA_memcpy(data, (pContext->pFileDesc + pContext->dataOffset), aSize);
        *pSize = aSize;
        err = M4WAR_NO_DATA_YET;
    }
    else
    {
        M4OSA_memcpy(data, (pContext->pFileDesc + pContext->dataOffset), aSize);
        err = M4NO_ERROR;
    }

    pContext->dataOffset += aSize;
    return err;
}

/**
 ******************************************************************************
 * @brief      This function seeks at the provided position in the core file
 *             reader (selected by its 'context'). The position is related to
 *             the seekMode parameter it can be either from the beginning, from
 *             the end or from the current postion.
 * @note       If this function returns an error the current position pointer
 *             in the file must not change. Else the current
 *             position pointer must be updated.
 * @param      context: (IN/OUT) Context of the core file reader
 * @param      seekMode: (IN) Seek access mode
 * @param      position: (IN/OUT) Position in the file
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_BAD_CONTEXT: provided context is not a valid one
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4ERR_FILE_INVALID_POSITION: the position cannot be reached
 ******************************************************************************
*/
M4OSA_ERR M4OSA_fileReadRamSeek( M4OSA_Context context,
                                 M4OSA_FileSeekAccessMode seekMode,
                                 M4OSA_FilePosition* position )
{
    M4OSA_FileReaderRam_Context* pContext=(M4OSA_FileReaderRam_Context*)context;
    M4OSA_ERR err = M4NO_ERROR;

    /* Check input parameters */
    if(context == M4OSA_NULL || seekMode == M4OSA_NULL || position == M4OSA_NULL)
    {
        return M4ERR_PARAMETER;
    }
    if (pContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;
    }

    /* */
    switch(seekMode)
    {
        case M4OSA_kFileSeekBeginning:
            /* Check if position is reachable and update dataOffset */
            if(((M4OSA_UInt32)(*position) <= pContext->dataSize) && (*position >= 0))
            {
                pContext->dataOffset = *position;
                *position = pContext->dataOffset;
                err = M4NO_ERROR;
            }
            else
            {
                err = M4ERR_FILE_INVALID_POSITION;
            }
            break;

        case M4OSA_kFileSeekEnd:
            /* Check if position is reachable and update dataOffset */
            if(((M4OSA_Int32)(pContext->dataSize) + *position >= 0) && (*position <= 0))
            {
                pContext->dataOffset = pContext->dataSize + *position;
                *position = pContext->dataOffset;
                err = M4NO_ERROR;
            }
            else
            {
                err = M4ERR_FILE_INVALID_POSITION;
            }
            break;

        case M4OSA_kFileSeekCurrent:
            /* Check if position is reachable and update dataOffset */
            if((*position + (M4OSA_Int32)(pContext->dataOffset) >= 0) &&
               (*position + (M4OSA_Int32)(pContext->dataOffset) <=
               (M4OSA_Int32)pContext->dataSize))
            {
                pContext->dataOffset += *position;
                *position = pContext->dataOffset;
                err = M4NO_ERROR;
            }
            else
            {
                err = M4ERR_FILE_INVALID_POSITION;
            }
            break;

        default:
            err = M4ERR_PARAMETER;
            break;
    }

    return err;
}

/**
 ******************************************************************************
 * @brief      This function asks the core file reader to close the file
 *             (associated to the context).
 * @note       The context of the core file reader is freed.
 * @param      context: (IN/OUT) Context of the core file reader
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_BAD_CONTEXT: provided context is not a valid one
 * @return     M4ERR_ALLOC: there is no more memory available
 ******************************************************************************
*/
M4OSA_ERR M4OSA_fileReadRamClose(M4OSA_Context context)
{
    M4OSA_FileReaderRam_Context* pContext=(M4OSA_FileReaderRam_Context*)context;

    /* Check input parameters */
    if(context == M4OSA_NULL)
    {
        return M4ERR_PARAMETER;
    }
    if (pContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;
    }

    pContext->IsOpened = M4OSA_FALSE;

    free(pContext);

    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * @brief      This function asks the core file reader to return the value
 *             associated with the optionID. The caller is responsible for
 *             allocating/de-allocating the memory of the value field.
 * @note       'value' must be cast according to the type related to the
 *             optionID As the caller is responsible for
 *             allocating/de-allocating the 'value' field, the callee must copy
 *             this field to its internal variable.
 * @param      context: (IN/OUT) Context of the core file reader
 * @param      optionID: (IN) ID of the option
 * @param      value: (OUT) Value of the option
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_BAD_CONTEXT: provided context is not a valid one
 * @return     M4ERR_BAD_OPTION_ID: the optionID is not a valid one
 * @return     M4ERR_NOT_IMPLEMENTED: this option is not implemented
 ******************************************************************************
*/
M4OSA_ERR M4OSA_fileReadRamGetOption( M4OSA_Context context,
                                      M4OSA_FileReadOptionID optionID,
                                      M4OSA_DataOption* optionValue )
{
    M4OSA_FileReaderRam_Context* pContext=(M4OSA_FileReaderRam_Context*)context;
    M4OSA_ERR err=M4NO_ERROR;

    /* Check input parameters */
    if(context == M4OSA_NULL)
    {
        return M4ERR_PARAMETER;
    }
    if (pContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;
    }

    switch(optionID)
    {
        case M4OSA_kFileReadGetFileSize:
            (*(M4OSA_UInt32 *)optionValue) = (pContext->dataSize);
            break;

        case M4OSA_kFileReadIsEOF:
            if(pContext->dataOffset == pContext->dataSize)
            {
                (*(M4OSA_UInt8 *)optionValue) = M4OSA_TRUE;
            }
            else
            {
                (*(M4OSA_UInt8 *)optionValue) = M4OSA_FALSE;
            }
            break;

        case M4OSA_kFileReadGetFileAttribute:
            err = M4ERR_NOT_IMPLEMENTED;
            break;

        case M4OSA_kFileReadGetURL:
            err = M4ERR_NOT_IMPLEMENTED;
            break;

        case M4OSA_kFileReadGetFilePosition :
            (*(M4OSA_UInt32 *)optionValue) = pContext->dataOffset;
            break;

        default:
            err = M4ERR_BAD_OPTION_ID;
            M4OSA_TRACE1_1("M4OSA_fileReadRamGetOption invalid option ID 0x%x",
                                                                      optionID);
            break;
    }

    return err;
}

/**
 ***************************************************************************
 * @fn         M4OSA_ERR M4OSA_fileReadSetOption (M4OSA_Context context,
 *                                                M4OSA_OptionID optionID,
 *                                                M4OSA_DataOption optionValue))
 * @brief      This function asks the core file reader to set the value associated with the optionID.
 *             The caller is responsible for allocating/de-allocating the memory of the value field.
 * @note       As the caller is responsible for allocating/de-allocating the 'value' field, the callee must copy this field
 *             to its internal variable.
 * @param      context: (IN/OUT) Context of the core file reader
 * @param      optionID: (IN) ID of the option
 * @param      value: (IN) Value of the option
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_BAD_CONTEXT: provided context is not a valid one
 * @return     M4ERR_BAD_OPTION_ID: the optionID is not a valid one
 * @return     M4ERR_READ_ONLY: this option is a read only one
 * @return     M4ERR_NOT_IMPLEMENTED: this option is not implemented
 ***************************************************************************
*/
M4OSA_ERR M4OSA_fileReadRamSetOption( M4OSA_Context context,
                                      M4OSA_FileReadOptionID optionID,
                                      M4OSA_DataOption optionValue )
{
   return M4ERR_NOT_IMPLEMENTED;
}

