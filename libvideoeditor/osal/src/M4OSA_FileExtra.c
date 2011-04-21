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
* @file         M4OSA_FileExtra.c
* @brief        File extra for Android
* @note         This file implements a set of basic functions to handle file
*               itself.
************************************************************************
*/

#include "M4OSA_Debug.h"
#include "M4OSA_FileCommon.h"
#include "M4OSA_FileCommon_priv.h"
#include "M4OSA_FileExtra.h"
#include "M4OSA_FileReader.h"
#include "M4OSA_FileWriter.h"

#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statfs.h>



/**
 ************************************************************************
 * @brief      This function deletes the provided URL.
 * @note
 * @param      pUrl: (IN) URL of the file to delete
 * @param      fileModeAccess: (IN) File mode access
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_NOT_IMPLEMENTED: the URL does not match with the supported
 *             file
 ************************************************************************
*/
M4OSA_ERR M4OSA_fileExtraDelete(const M4OSA_Char* pUrl)
{

    M4OSA_Int32  err;
#ifdef UTF_CONVERSION
    M4OSA_Void* tempConversionBuf;
    M4OSA_UInt32 tempConversionSize = 1000;    /*size of the decoded buffer,
                                                 can be increase if necessary*/
#endif /* UTF_CONVERSION */

    M4OSA_TRACE1_1("M4OSA_fileExtraDelete\t\tM4OSA_Char* %s", pUrl);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pUrl, M4ERR_PARAMETER,
                                   "M4OSA_fileExtraDelete: pUrl is M4OSA_NULL");

#ifdef UTF_CONVERSION
    /*FB: to test the UTF16->UTF8 conversion into Video Artist*/
    /*Convert the URL from UTF16 to UTF8*/
    tempConversionBuf = (M4OSA_Char*)M4OSA_32bitAlignedMalloc(tempConversionSize +1, 0,
                                                 (M4OSA_Char*)"conversion buf");
    if(tempConversionBuf == M4OSA_NULL)
    {
        M4OSA_TRACE1_0("Error when allocating conversion buffer\n");
        return M4ERR_PARAMETER;
    }
    M4OSA_ToUTF8_OSAL((M4OSA_Void*)pUrl, tempConversionBuf, &tempConversionSize);
    ((M4OSA_Char*)tempConversionBuf)[tempConversionSize ] = '\0';

    printf("remove file %s\n", tempConversionBuf);

    /*Open the converted path*/
    err = remove (tempConversionBuf);
    /*Free the temporary decoded buffer*/
    free(tempConversionBuf);
#else
    err = remove((const char *)pUrl);
#endif /* UTF_CONVERSION */

    if(-1 == err)
    {
        M4OSA_DEBUG(M4ERR_PARAMETER,
                          "M4OSA_fileExtraDelete: Cannot remove the input url");
        return M4ERR_PARAMETER;
    }

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function copies the file located by 'pSrcUrl' to 'pDstUrl'.
 * @note
 * @param      pSrcUrl: (IN) source URL
 * @param      pDstUrl: (IN) Destination URL
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_NOT_IMPLEMENTED: the URL does not match with the supported
 *             file
 ************************************************************************
*/
M4OSA_ERR M4OSA_fileExtraCopy(M4OSA_Char* pSrcUrl, M4OSA_Char* pDstUrl)
{
    M4OSA_Context pInputFileContext    = M4OSA_NULL;
    M4OSA_Context pOutputFileContext= M4OSA_NULL;

    M4OSA_ERR       err;
    M4OSA_UInt32    uiSizeRead = BUFFER_COPY_SIZE;
    M4OSA_MemAddr32 copy_buffer;

    M4OSA_TRACE1_2("M4OSA_fileExtraDelete\t\tM4OSA_Char* %s\tM4OSA_Char* %s",
                                                              pSrcUrl, pDstUrl);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pDstUrl, M4ERR_PARAMETER,
                                  "M4OSA_fileExtraCopy: pDstUrl is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pSrcUrl, M4ERR_PARAMETER,
                                  "M4OSA_fileExtraCopy: pSrcUrl is M4OSA_NULL");

    /* Open input file */
    err = M4OSA_fileReadOpen(&pInputFileContext, pSrcUrl, M4OSA_kFileRead);
    if(M4NO_ERROR != err)
    {
        M4OSA_DEBUG(err, "M4OSA_fileExtraCopy: M4OSA_fileReadOpen");
        return err;
    }

    /* Open output file */
    err = M4OSA_fileWriteOpen(&pOutputFileContext, pDstUrl,
                        M4OSA_kFileWrite|M4OSA_kFileCreate);
    if(M4NO_ERROR != err)
    {
        M4OSA_DEBUG(err, "M4OSA_fileExtraCopy: M4OSA_fileWriteOpen");
        return err;
    }

    /* Allocate buffer */
    copy_buffer = M4OSA_32bitAlignedMalloc(BUFFER_COPY_SIZE, M4OSA_FILE_EXTRA,
                               (M4OSA_Char*)"M4OSA_fileExtraCopy: copy buffer");
    if(M4OSA_NULL == copy_buffer)
    {
        M4OSA_DEBUG(M4ERR_ALLOC, "M4OSA_fileExtraCopy");
        return M4ERR_ALLOC;
    }

    /* Copy input file to output file using copy buffer */
    while (1)
    {
        /* Load data into copy buffer */
        err = M4OSA_fileReadData(pInputFileContext,
                                      (M4OSA_MemAddr8)copy_buffer, &uiSizeRead);
        if(M4NO_ERROR == err)
        {
            /* Write data to output file */
            err = M4OSA_fileWriteData(pOutputFileContext,
                                       (M4OSA_MemAddr8)copy_buffer, uiSizeRead);
            if(M4NO_ERROR != err)
            {
                break;
            }
        }
        else if (M4WAR_NO_DATA_YET == err)
        {
            /* no more data to copy, end of file reached */
            err = M4OSA_fileWriteData(pOutputFileContext,
                                       (M4OSA_MemAddr8)copy_buffer, uiSizeRead);
            break;
        }
        else
        {
            break; /* an error occur */
        }
    }

    /* Free copy buffer */
    free(copy_buffer);

    err = M4OSA_fileWriteClose(pOutputFileContext);
    if(M4NO_ERROR != err)
    {
        M4OSA_DEBUG(err, "M4OSA_fileExtraCopy: M4OSA_fileWriteClose");
    }

    err = M4OSA_fileReadClose(pInputFileContext);
    if(M4NO_ERROR != err)
    {
        M4OSA_DEBUG(err, "M4OSA_fileExtraCopy: M4OSA_fileReadClose");
    }

    return err;
}


/**
 ************************************************************************
 * @brief      This function renames the 'pSrcUrl' to 'pDstUrl'.
 * @note
 * @param      pSrcUrl: (IN) source URL
 * @param      pDstUrl: (IN) Destination URL
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_NOT_IMPLEMENTED: the URL does not match with the supported
 *             file
 ************************************************************************
*/
M4OSA_ERR M4OSA_fileExtraRename(M4OSA_Char* pSrcUrl, M4OSA_Char* pDstUrl)
{
    M4OSA_ERR    err;
    M4OSA_Int32 iValue;
    M4OSA_Char*    pSrcFilename = M4OSA_NULL;
    M4OSA_Char*    pDstFilename = M4OSA_NULL;

    M4OSA_TRACE1_2("M4OSA_fileExtraRename\t\tM4OSA_Char* %s\tM4OSA_Char* %s",
                                                              pSrcUrl, pDstUrl);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pDstUrl, M4ERR_PARAMETER,
                                "M4OSA_fileExtraRename: pSrcUrl is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pSrcUrl, M4ERR_PARAMETER,
                                "M4OSA_fileExtraRename: pDstUrl is M4OSA_NULL");

    err = M4OSA_fileCommonGetFilename(pSrcUrl, &pSrcFilename);
    if(M4NO_ERROR != err)
    {
        M4OSA_DEBUG(err, "M4OSA_fileExtraRename: M4OSA_fileCommonGetFilename");
        return err;
    }

    err = M4OSA_fileCommonGetFilename(pDstUrl, &pDstFilename);
    if(M4NO_ERROR != err)
    {
        free(pSrcFilename);
        M4OSA_DEBUG(err, "M4OSA_fileExtraRename: M4OSA_fileCommonGetFilename");
        return err;
    }

    /* Rename file */
    iValue = rename((const char *)pSrcFilename, (const char *)pDstFilename);
    if (0 != iValue)
    {
    /*
        What error code shall be returned ? From MSDN:
        Each of these functions returns 0 if it is successful. On an error, the
        function  returns a nonzero value and sets errno to one of the following
        values:
        - EACCES: File or directory specified by newname already exists or could
        not be created (invalid path); or oldname is a directory and newname
        specifies a different path.
        - ENOENT: File or path specified by oldname not found.
        - EINVAL: Name contains invalid characters.
        For other possible return values, see _doserrno, _errno, syserrlist, and
            _sys_nerr. */
        M4OSA_DEBUG(M4ERR_PARAMETER, "M4OSA_fileExtraRename: rename failed");
        return M4ERR_PARAMETER;
    }

    free(pDstFilename);
    free(pSrcFilename);

    return M4NO_ERROR;
}



/**
 ************************************************************************
 * @brief      This function changes the current directory to the specified new
 *             directory 'url'.
 * @note
 * @param      pUrl: (IN) Directory to which current directory to be changed
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_NOT_IMPLEMENTED: the URL does not match with the supported
 *             file
 ************************************************************************
*/

M4OSA_ERR M4OSA_fileExtraChangeCurrentDir(const M4OSA_Char* pUrl)
{
    M4OSA_ERR    err;
    M4OSA_Char* pFileName = M4OSA_NULL;
    M4OSA_Int32 iValue = 0;

    M4OSA_TRACE1_1("M4OSA_fileExtraChangeCurrentDir\t\tM4OSA_Char* %s", pUrl);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pUrl, M4ERR_PARAMETER,
                         "M4OSA_fileExtraChangeCurrentDir: pUrl is M4OSA_NULL");

    err = M4OSA_fileCommonGetFilename((M4OSA_Char*)pUrl, &pFileName);
    if(M4NO_ERROR != err)
    {
        M4OSA_DEBUG(err,
                "M4OSA_fileExtraChangeCurrentDir: M4OSA_fileCommonGetFilename");
        return err;
    }

    iValue = chdir((char*)pFileName);

    if (iValue != 0)
    {
    /*
    What error code shall be returned ? From MSDN:
    Each of these functions returns a value of 0 if successful. A return
    value of -1 indicates that the specified path could not be found, in
    which case errno is set to ENOENT.*/
        M4OSA_DEBUG(M4ERR_PARAMETER,
                               "M4OSA_fileExtraChangeCurrentDir: chdir failed");
        return(M4ERR_PARAMETER);
    }

    free(pFileName);

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function creates a new directory to the specified 'url'.
 * @note
 * @param      pUrl: (IN) Path to create new directory with name
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_NOT_IMPLEMENTED: the URL does not match with the supported
 *             file
 ************************************************************************
*/
M4OSA_ERR M4OSA_fileExtraCreateDir(const M4OSA_Char* pUrl)
{
    M4OSA_Int32        err;

    M4OSA_TRACE2_1("M4OSA_fileExtraCreateDir %s", pUrl);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pUrl, M4ERR_PARAMETER,
                                "M4OSA_fileExtraCreateDir: pUrl is M4OSA_NULL");

    err = mkdir((char*)pUrl, S_IRWXU | S_IRWXG | S_IRWXO);

    if( err < 0 )
    {
           return M4OSA_ERR_CREATE(M4_ERR, M4OSA_FILE_EXTRA, errno);
    }

    return M4NO_ERROR;
}


/**
 ************************************************************************
 * @brief      This function removes the current directory.
 * @note
 * @param      pUrl: (IN) Path of directory with name
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_NOT_IMPLEMENTED: the URL does not match with the supported
 *             file
 ************************************************************************
*/
M4OSA_ERR M4OSA_fileExtraRemoveDir(const M4OSA_Char* pUrl)
{
    M4OSA_Int32    err;

    M4OSA_TRACE2_1("M4OSA_fileExtraRemoveDir %s", pUrl);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pUrl, M4ERR_PARAMETER,
                                "M4OSA_fileExtraRemoveDir: pUrl is M4OSA_NULL");

    err = rmdir((char*)pUrl);
    if(err < 0 )
    {
        M4OSA_DEBUG(M4ERR_PARAMETER, "M4OSA_fileExtraRemoveDir failed");
        return M4OSA_ERR_CREATE(M4_ERR, M4OSA_FILE_EXTRA, errno);
    }

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      retrieves the free space.
 * @note
 * @param      pUrl: (IN) root directory
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_NOT_IMPLEMENTED: the URL does not match with the supported
 *             file
 ************************************************************************
*/
M4OSA_UInt32 M4OSA_fileExtraGetFreeSpace(const M4OSA_Char* pUrl)
{
    M4OSA_UInt32 size = 0;
    struct statfs stat;

    if ( M4OSA_NULL != pUrl )
    {
        if (0 == statfs( (const char *)pUrl, &stat ))
        {
            if ((stat.f_bfree * stat.f_bsize) > M4OSA_UINT32_MAX)
            {
                size = M4OSA_UINT32_MAX;
            }
            else
            {
                size = (M4OSA_UInt32)(stat.f_bfree * stat.f_bsize);
            }
        }
    }

    return (size);
}

/**
 ************************************************************************
 * @brief      This function gets the total space
 * @note
 * @param      pUrl: (IN) Path of directory with name
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_NOT_IMPLEMENTED: the URL does not match with the supported
 *             file
 ************************************************************************
*/
M4OSA_UInt32 M4OSA_fileExtraGetTotalSpace(const M4OSA_Char* pUrl)
{
    M4OSA_UInt32 size = 0;
    struct statfs stat;

    if ( M4OSA_NULL != pUrl )
    {
        if (0 == statfs( (const char *)pUrl, &stat ))
        {
             if ((stat.f_blocks * stat.f_bsize) > M4OSA_UINT32_MAX)
            {
                size = M4OSA_UINT32_MAX;
            }
            else
            {
                size = (M4OSA_UInt32)(stat.f_blocks * stat.f_bsize);
            }
        }
    }

    return (size);
}

/**
 ************************************************************************
 * @brief      This function retrieve the file type (Directory or file).
 * @note
 * @param      pUrl: (IN) Path of directory with name
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_NOT_IMPLEMENTED: the URL does not match with the supported
 *             file
 ************************************************************************
*/
M4OSA_EntryType M4OSA_fileExtraGetType(const M4OSA_Char* pUrl)
{
    M4OSA_EntryType type = M4OSA_TypeInvalid;
    struct stat fileStat;

    if ( M4OSA_NULL != pUrl )
    {
        if (0 == stat( (const char *)pUrl, &fileStat))
        {
            if ( S_ISDIR( fileStat.st_mode ) )
            {
                type = M4OSA_TypeDir;
            }
            else
            {
                type = M4OSA_TypeFile;
            }
        }
    }

    return (type);
}


/**
 ************************************************************************
 * @brief      This function truncate a file.
 *               the file must be previously opened in write mode
 * @note       the position pointer in the file is set to the beginning
 *               of the file after the truncate
 * @param      context: (IN) media context
 * @param      length: (IN) length of the file after truncation
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_ALLOC: there is no more memory available
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 ************************************************************************
*/
M4OSA_ERR M4OSA_fileExtrafTruncate(M4OSA_Context context, M4OSA_FilePosition length)
{
    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_UInt16 result = M4OSA_FALSE;
    M4OSA_FileContext* pFileContext = context;

    M4OSA_DEBUG_IF2(M4OSA_NULL == context, M4ERR_PARAMETER,
                             "M4OSA_fileExtrafTruncate: context is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == length, M4ERR_PARAMETER,
                              "M4OSA_fileExtrafTruncate: length is M4OSA_NULL");

    result = ftruncate(pFileContext->file_desc->_file, length);

    if(result != 0)
    {
        err = errno;
        M4OSA_TRACE1_1("SetEndOfFile returns err: 0x%x\n", err);
        return M4OSA_ERR_CREATE(M4_ERR, M4OSA_FILE_EXTRA, err);
    }
    return M4NO_ERROR;
}


