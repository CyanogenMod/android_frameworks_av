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

 * @brief        File reader from RAM
 * @note         This file implements functions to read a "file" stored in RAM.
******************************************************************************
*/

#include "M4OSA_Debug.h"
#include "M4OSA_FileWriterRam.h"
#include "M4OSA_FileReaderRam.h"
#include "M4OSA_Memory.h"


/**
 ******************************************************************************
 * structure    M4OSA_FileWriteRam_Context
 * @brief        This structure defines the File writer context (private)
 * @note        This structure is used for all File writer calls to store the context
 ******************************************************************************
*/
typedef struct
{
    M4OSA_MemAddr8    pFileDesc;    /* Pointer on file data */
    M4OSA_UInt32    dataSize;    /* Size of data to write */
    M4OSA_UInt32    dataOffset;    /* Actual offset */
    M4OSA_Bool        IsOpened;    /* Micro state machine */
    M4OSA_UInt32    bufferSize;    /* Actual used size inside the buffer */
} M4OSA_FileWriterRam_Context;


/**
 ******************************************************************************
 * @brief    This method "opens" the provided fileDescriptor (in fact address)
 *            and returns its context.
 * @param    pContext:    (OUT) File writer context.
 * @param    pFileDescriptor :    (IN) File Descriptor of the input file.
 * @param    FileModeAccess :    (IN) File mode access.
 * @return    M4NO_ERROR: there is no error
 * @return    M4ERR_PARAMETER    pContext or fileDescriptor is NULL
 * @return    M4ERR_ALLOC    there is no more memory available
 * @return    M4ERR_FILE_BAD_MODE_ACCESS    the file mode access is not correct
 * @return    M4ERR_FILE_NOT_FOUND The file can not be opened.
 ******************************************************************************
*/
M4OSA_ERR M4OSA_fileWriteRamOpen(M4OSA_Context* context, M4OSA_Void* fileDescriptor,
                                                    M4OSA_UInt32 fileModeAccess)
{
    M4OSA_FileWriterRam_Context* pContext=M4OSA_NULL;
    M4OSA_FileWriterRam_Descriptor* pDescriptor = fileDescriptor;
    M4OSA_Int32 aFileDesc=-1;
    M4OSA_ERR   err=M4NO_ERROR;

    /*    Check input parameters */
    if(context == M4OSA_NULL)
    {
        return M4ERR_PARAMETER;
    }
    *context = M4OSA_NULL;
    if(fileDescriptor == M4OSA_NULL)
    {
        return M4ERR_PARAMETER;
    }

    /*    Allocate memory for the File writer context. */
    pContext = (M4OSA_FileWriterRam_Context *)M4OSA_32bitAlignedMalloc(sizeof(M4OSA_FileWriterRam_Context),
                          M4OSA_FILE_WRITER, (M4OSA_Char*)"Context allocation");
    if(pContext == M4OSA_NULL)
    {
        return M4ERR_ALLOC;
    }

    if ((fileModeAccess & M4OSA_kFileWrite))
    {
        pContext->pFileDesc = (M4OSA_MemAddr8)(pDescriptor->pFileDesc);
        pContext->dataSize = (M4OSA_Int32)(pDescriptor->dataSize);
        pContext->dataOffset = 0;
        pContext->bufferSize = 0;
        pContext->IsOpened = M4OSA_TRUE;
    }
    else
    {
        err = M4ERR_FILE_BAD_MODE_ACCESS;
    }
    if (M4NO_ERROR != err)
    {
          if (M4OSA_NULL != pContext)
        {
            free(pContext);
        }
        *context=M4OSA_NULL;
    }
    else
    {
        *context = pContext;
    }

    return err;
}

/**
 ******************************************************************************
 * @brief    This method writes the 'size' bytes stored at 'data' memory at the end
 *            of the file selected by its context.
 *            The caller is responsible for allocating/de-allocating the memory for 'data' parameter.
 *            Moreover, the data pointer must be allocated to store at least 'size' bytes.
 * @param    pContext:    (IN) File writer context.
 * @param    pData :    (IN) Data pointer of the written data.
 * @param    Size :    (IN) Size of the data to write (in bytes).
 * @return    M4NO_ERROR: there is no error
 * @return    M4ERR_PARAMETER     pData is NULL
 * @return    M4ERR_ALLOC    there is no more memory available
 * @return    M4ERR_BAD_CONTEXT    provided context is not a valid one.
 ******************************************************************************
*/
M4OSA_ERR M4OSA_fileWriteRamData(M4OSA_Context context,M4OSA_MemAddr8 data, M4OSA_UInt32 Size)
{
    M4OSA_FileWriterRam_Context* pContext=(M4OSA_FileWriterRam_Context*)context;
    M4OSA_ERR err=M4NO_ERROR;

    /*    Check input parameters */
    if(context == M4OSA_NULL || data == M4OSA_NULL)
    {
        return M4ERR_PARAMETER;
    }

    if (pContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;    /* The context can not be correct */
    }

    /* Check if there is enough room to write or not */
    if (pContext->dataOffset + Size < pContext->dataSize )
    {
        M4OSA_memcpy((pContext->pFileDesc + pContext->dataOffset), data, Size);
        pContext->dataOffset += Size;
        if(pContext->dataOffset> pContext->bufferSize) pContext->bufferSize = pContext->dataOffset;
        err = M4NO_ERROR;
    }
    else
    {
        err = M4ERR_FILE_INVALID_POSITION;
    }

    return err;
}

/**
 ******************************************************************************
 * @brief    This method seeks at the provided position in the core file writer (selected by its 'context').
 *            The position is related to the seekMode parameter it can be either :
 *                From the beginning (position MUST be positive) : end position = position
 *                From the end (position MUST be negative) : end position = file size + position
 *                From the current position (signed offset) : end position = current position + position.
 * @param    pContext:    (IN) File reader context.
 * @param    SeekMode :    (IN) Seek access mode.
 * @param    pPosition :    (IN) Position in the file.
 * @return    M4NO_ERROR: there is no error
 * @return    M4ERR_PARAMETER    Seekmode or fileDescriptor is NULL
 * @return    M4ERR_ALLOC    there is no more memory available
 * @return    M4ERR_BAD_CONTEXT    provided context is not a valid one.
 * @return    M4ERR_FILE_INVALID_POSITION the position cannot be reached.
 ******************************************************************************
*/
M4OSA_ERR M4OSA_fileWriteRamSeek(M4OSA_Context context, M4OSA_FileSeekAccessMode SeekMode,
                                                   M4OSA_FilePosition* position)
{
    M4OSA_FileWriterRam_Context* pContext=(M4OSA_FileWriterRam_Context*)context;
    M4OSA_ERR err=M4NO_ERROR;

    /*    Check input parameters */
    if(context == M4OSA_NULL || SeekMode == M4OSA_NULL || position == M4OSA_NULL)
    {
        return M4ERR_PARAMETER;
    }

    if (pContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;    /* The context can not be correct */
    }

    /* Go to the desired position */
    switch(SeekMode)
    {
        case M4OSA_kFileSeekBeginning:
            /* Check if position is reachable and update dataOffset */
            if (((*position) >= 0) && ((M4OSA_UInt32)(*position) <= pContext->dataSize))
            {
                pContext->dataOffset = *position;
                err = M4NO_ERROR;
            }
            else
            {
                err = M4ERR_FILE_INVALID_POSITION;
            }
            break;

        case M4OSA_kFileSeekEnd:
            /* Check if position is reachable and update dataOffset */
            if ((*position) < 0)
            {
                if (pContext->dataSize >= (M4OSA_UInt32)(-(*position)))
                {
                    pContext->dataOffset = (M4OSA_UInt32) (pContext->pFileDesc + pContext->dataSize + (*position));
                    err = M4NO_ERROR;
                }
                else
                {
                    err = M4ERR_FILE_INVALID_POSITION;
                }
            }
            else if ((*position) == 0)
            {
                 pContext->dataOffset = (M4OSA_UInt32)(pContext->pFileDesc + pContext->dataSize);
                 err = M4NO_ERROR;
            }
            else
            {
                err = M4ERR_FILE_INVALID_POSITION;
            }
            break;

        case M4OSA_kFileSeekCurrent:
            /* Check if position is reachable and update dataOffset */
            if ((*position) < 0)
            {
                if (pContext->dataOffset >= (M4OSA_UInt32)(-(*position)))
                {
                    pContext->dataOffset = (M4OSA_UInt32) (pContext->dataOffset + (*position));
                    err = M4NO_ERROR;
                }
                else
                {
                    err = M4ERR_FILE_INVALID_POSITION;
                }
            }
            else
            {
                if (pContext->dataSize >= (M4OSA_UInt32)(pContext->dataOffset + (*position)))
                {
                    pContext->dataOffset = (M4OSA_UInt32) (pContext->dataOffset + (*position));
                    err = M4NO_ERROR;
                }
                else
                {
                    err = M4ERR_FILE_INVALID_POSITION;
                }
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
 * @brief    This method asks the core file writer to close the file (associated to the context).
 *            The context of the core file reader must be freed.
 * @param    pContext:    (IN) File reader context.
 * @return    M4NO_ERROR: there is no error
 * @return    M4ERR_BAD_CONTEXT    provided context is not a valid one.
 ******************************************************************************
*/
M4OSA_ERR M4OSA_fileWriteRamClose(M4OSA_Context context)
{
    M4OSA_FileWriterRam_Context* pContext=(M4OSA_FileWriterRam_Context*)context;
    M4OSA_ERR err=M4NO_ERROR;

    /*    Check input parameters */
    if(pContext == M4OSA_NULL)
    {
        return M4ERR_PARAMETER;
    }

    if (pContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;    /* The context can not be correct */
    }

    pContext->IsOpened = M4OSA_FALSE;

    /* Free the context */
    free(pContext);

    /*    Return error */
    return err;
}

/**
 ******************************************************************************
 * @brief    This method asks the core file writer to flush the pending data
 *            to the file (associated to the context).
 *            All pending written data are written in the file.
 * @param    pContext:    (IN) File reader context.
 * @return    M4NO_ERROR: there is no error
 * @return    M4ERR_BAD_CONTEXT    provided context is not a valid one.
 ******************************************************************************
*/
M4OSA_ERR M4OSA_fileWriteRamFlush(M4OSA_Context context)
{
    M4OSA_FileWriterRam_Context* pContext=(M4OSA_FileWriterRam_Context*)context;

    /*    Check input parameters */
    if(context == M4OSA_NULL)
    {
        return M4ERR_PARAMETER;
    }

    if (pContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;    /* The context can not be correct */
    }

    /*
     * DO NOTHING */

    /**
     *    Return without error */
    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * @brief    This method asks the core file writer to set the value associated with the optionID.
 *            The caller is responsible for allocating/de-allocating the memory of the value field.
 * @note    The options handled by the component depend on the implementation of the component.
 * @param    pContext:    (IN) Execution context.
 * @param    OptionId :    (IN) Id of the option to set.
 * @param    OptionValue :    (IN) Value of the option.
 * @return    M4NO_ERROR: there is no error
 * @return    M4ERR_BAD_CONTEXT    pContext is NULL
 * @return    M4ERR_READ_ONLY The option is not implemented yet.
 * @return    M4ERR_BAD_OPTION_ID the option id is not valid.
 * @return    M4ERR_NOT_IMPLEMENTED The option is not implemented yet.
 ******************************************************************************
*/
M4OSA_ERR M4OSA_fileWriteRamSetOption(M4OSA_Context context,
                                      M4OSA_OptionID OptionID,
                                      M4OSA_DataOption OptionValue)
{
    M4OSA_FileWriterRam_Context* pContext=(M4OSA_FileWriterRam_Context*)context;

    /*    Check input parameters */
    if(context == M4OSA_NULL)
    {
        return M4ERR_PARAMETER;
    }

    if (pContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;    /**< The context can not be correct */
    }

    /*    Set the desired option if it is avalaible */
    switch(OptionID)
    {
        case M4OSA_kFileWriteGetReaderContext :    /* Get the file attribute*/
        case M4OSA_kFileWriteGetURL :            /* Get the directory + name of the file */
        case M4OSA_kFileWriteGetFilePosition :    /* Get file position */
            return M4ERR_READ_ONLY;
            break;

        case M4OSA_kFileWriteGetAttribute :
            /**
             * Get the reader context for read & write file. It is NULL if the file is opened
             * with write attribute only */
            return M4ERR_NOT_IMPLEMENTED;

        default :                                /* Bad option ID */
            return    M4ERR_BAD_OPTION_ID;
    }

    /*    Return without error */
    return M4NO_ERROR;
}

/**
 ******************************************************************************
 * @brief    This method asks the core file reader to return the value associated with the optionID.
 *            The caller is responsible for allocating/de-allocating the memory of the value field.
 * @note    The options handled by the component depend on the implementation of the component.
 * @param    pContext:    (IN) Execution context.
 * @param    OptionId :    (IN) Id of the option to set.
 * @param    pOptionValue :    (OUT) Value of the option.
 * @return    M4NO_ERROR: there is no error
 * @return    M4ERR_BAD_CONTEXT    pContext is NULL
 * @return    M4ERR_BAD_OPTION_ID the option id is not valid.
 * @return    M4ERR_ALLOC    there is no more memory available
 * @return    M4ERR_NOT_IMPLEMENTED The option is not implemented yet.
 ******************************************************************************
*/
M4OSA_ERR M4OSA_fileWriteRamGetOption(M4OSA_Context context,
                                      M4OSA_OptionID OptionID,
                                      M4OSA_DataOption* optionValue)
{
    M4OSA_FileWriterRam_Context* pContext=(M4OSA_FileWriterRam_Context*)context;
    M4OSA_ERR   err=M4NO_ERROR;

    /*    Check input parameters */
    if(context == M4OSA_NULL)
    {
        return M4ERR_PARAMETER;
    }

    if (pContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;    /**< The context can not be correct */
    }

    /*    Get the desired option if it is avalaible */
    switch(OptionID)
    {
        case M4OSA_kFileWriteGetFileSize:/* Get size of the file, limited to 32 bit size */
            (*(M4OSA_UInt32 *)optionValue) = (pContext->bufferSize);
            break;

        case M4OSA_kFileWriteGetURL :    /* Get the directory + name of the file */
            return M4ERR_NOT_IMPLEMENTED;

        default :                                /**< Bad option ID */
            return M4ERR_BAD_OPTION_ID;
            break;
    }

    /*    Return without error */
    return err;
}
