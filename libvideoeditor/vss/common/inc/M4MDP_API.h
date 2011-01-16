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
*************************************************************************
 * @file   M4MDP_API.h
 * @brief  Parser of metadata
 *
*************************************************************************
*/

#ifndef __M4MDP_API_H__
#define __M4MDP_API_H__

#ifdef __cplusplus
extern "C" {
#endif

#define MD4MDP_close M4MDP_close

#include "M4READER_Common.h"
#include "M4TOOL_VersionInfo.h"
#include "M4OSA_FileReader.h"
#include "M4OSA_FileWriter.h"

/*define the buffer size for content detection*/
#define M4MDP_INPUT_BUFFER_SIZE    8192

/**
 ************************************************************************
 * Public type of the M4MDP_osaFilePtrSt
 ************************************************************************
*/
typedef struct
{
    M4OSA_FileReadPointer*     m_pFileReaderFcts;
    M4OSA_FileWriterPointer* m_pFileWriterFcts;
} M4MDP_osaFilePtrSt;

/**
 ************************************************************************
 * Public type of the MDP execution context
 ************************************************************************
*/
typedef M4OSA_Void* M4MDP_Context;

/**
 ************************************************************************
 * Metadata Parser Errors & Warnings definition
 ************************************************************************
*/
#define M4WAR_MDP_MEDIATYPE_NOT_DETECTED        M4OSA_ERR_CREATE(M4_WAR, M4MDP, 0x000001)

#define    M4ERR_MDP_FATAL                            M4OSA_ERR_CREATE(M4_ERR, M4MDP, 0x000000)
#define    M4ERR_MDP_UNSUPPORTED_TAG_VERSION        M4OSA_ERR_CREATE(M4_ERR, M4MDP, 0x000001)
#define    M4ERR_MDP_UNSUPPORTED_ENCODING_TYPE        M4OSA_ERR_CREATE(M4_ERR, M4MDP, 0x000002)
#define    M4ERR_MDP_INIT_FAILED                    M4OSA_ERR_CREATE(M4_ERR, M4MDP, 0x000003)
#define    M4ERR_MDP_ASSET_PARSING_ERROR            M4OSA_ERR_CREATE(M4_ERR, M4MDP, 0x000004)
#define M4ERR_MDP_FILE_NOT_FOUND                M4OSA_ERR_CREATE(M4_ERR, M4MDP, 0x000005)
#define M4ERR_MDP_INVALID_PATH                    M4OSA_ERR_CREATE(M4_ERR, M4MDP, 0x000006)

/**
 ************************************************************************
 * Metadata parser FUNCTIONS
 ************************************************************************
*/

/**
 ************************************************************************
 * @brief    Getting the version of the metadata parser
 *            This function allows getting the version of the MDP library.
 *
 * @param    pVersionInfo    (OUT) Pointer on an allocated version info structure
 *                            After M4MDP_getVersion() successfully returns, this
 *                            structure is filled with the version numbers.
 *                            The structure must be allocated and further de-allocated
 *                            by the application.
 *
 * @return    M4NO_ERROR            No error
 * @return    M4ERR_PARAMETER        pVersionInfo is null (in DEBUG only)
 ************************************************************************
*/
M4OSA_ERR  M4MDP_getVersion(M4_VersionInfo* pVersionInfo);


/**
 ************************************************************************
 * @brief    Initializing the MDP
 *            This function initializes the MDP and allocates the MDP execution
 *            context and parses the metadata
 * @note    This function allocates the memory needed to store metadata in
 *            TAG ID3 V1&V2, ASF or 3gpp asset structure with the OSAL allocation
 *            function.
 *            This memory will be freed in M4MDP_cleanUp function
 *
 * @note    This function is synchronous.
 *
 * @param    pContext        (OUT)    Execution Context
 * @param    pFilePath        (IN)    Pointer to the multimedia file path
 * @param    pFileReaderFcts    (IN)    Pointer to a structure containing OSAL file reader
 *                                       functions pointers
 *
 * @return    M4NO_ERROR                        No error
 * @return    M4ERR_PARAMETER                    At least, one parameter is null (in DEBUG only)
 * @return    M4ERR_ALLOC                        There is no more memory available
 * @return    M4WAR_READER_NO_METADATA        The input file doesn't contain metadata
 * @return    M4ERR_UNSUPPORTED_MEDIA_TYPE    The input file is not recognized
 ************************************************************************
*/
M4OSA_ERR M4MDP_init(M4MDP_Context* pContext, M4OSA_Char* pFilePath,
                      M4OSA_FileReadPointer*    pFileReaderFcts);

/**
 ************************************************************************
 * @brief    This function frees the MDP execution context and all metadata
 *            structures already allocated by M4MDP_init
 *
 * @note    This function is synchronous.
 *
 * @param    pContext                (IN) Execution Context
 *
 * @return    M4NO_ERROR            No error
 * @return    M4ERR_PARAMETER        pContext is NULL. (in DEBUG only)
************************************************************************
*/
M4OSA_ERR M4MDP_cleanUp(M4MDP_Context pContext);

/**
 ************************************************************************
 * @brief    This function Initializes the meta data parser only once to check several files one
 *            after another.
 *
 * @note    This function is synchronous.
 *
 * @param    pContext                (IN) Execution Context
  * @param    pFileReaderFcts    (IN)    Pointer to a structure containing OSAL file reader
  *                                          functions pointers
*
 * @return    M4NO_ERROR            No error
 * @return    M4ERR_PARAMETER        pContext is NULL. (in DEBUG only)
************************************************************************
*/
M4OSA_ERR M4MDP_globalInit(M4MDP_Context* pContext, M4OSA_FileReadPointer*    pFileReaderFcts);

/**
 ************************************************************************
 * @brief    This function opens a file in the meta data parser
 *
 * @note    This function is synchronous.
 *
 * @param    pContext                (IN) Execution Context
 * @param    pFilePath        (IN)    Pointer to the multimedia file path
  *
 * @return    M4NO_ERROR            No error
 * @return    M4ERR_PARAMETER        pContext is NULL. (in DEBUG only)
************************************************************************
*/
M4OSA_ERR M4MDP_open(M4MDP_Context* pContext, M4OSA_Char* pFilePath);

/**
 ************************************************************************
 * @brief    This function closes a file in the meta data parser
 *
 * @note    This function is synchronous.
 *
 * @param    pContext                (IN) Execution Context
  *
 * @return    M4NO_ERROR            No error
 * @return    M4ERR_PARAMETER        pContext is NULL. (in DEBUG only)
************************************************************************
*/
M4OSA_ERR M4MDP_close(M4MDP_Context* pContext);


/**
 ************************************************************************
 * @brief    The function allows the retrieval of all fields of the
 *            M4_MetaDataFields structure
 *            It basically sets M4_MetaDataFields structure fields pointers to
 *            the corresponding already retrieved metadata
 *
 * @note    If metadata are retrieved from an MP3 or an AAC files, and both
 *            TAG ID3 V1 and V2 are present, then, priority is for metadata of TAG ID3 V2
 *
 * @note    This function is synchronous.
 * @note    This function is used specially by the music manager project
 *
 * @param    pContext        (IN) Execution Context
 * @param    pMetadata        (OUT) Pointer to M4_MetaDataFields structure
 *
 * @return    M4NO_ERROR                        No error
 * @return    M4ERR_PARAMETER                    pContext or pMetadata is NULL. (in DEBUG only)
 * @return    M4WAR_READER_NO_METADATA        The input file doesn't contain metadata
 ************************************************************************
*/
M4OSA_ERR M4MDP_getMetadata(M4MDP_Context pContext, M4_MetaDataFields* pMetadata);

/**
 ************************************************************************
 * @brief    This function returns the audio and video media type
 *
 * @note    This function is synchronous.
 * @note    This function is used specially by the music manager project
 *
 * @param    pContext        (IN)    Execution Context
 * @param    pAudio            (OUT)    Audio media type pointer
 * @param    pVideo            (OUT)    Video media type pointer
 *
 * @return    M4NO_ERROR            No error
 * @return    M4ERR_PARAMETER        At least one parameter is NULL. (in DEBUG only)
 ************************************************************************
*/
M4OSA_ERR M4MDP_getStreamsType(M4MDP_Context pContext,M4_StreamType* pAudio,M4_StreamType* pVideo);


/**
 ************************************************************************
 * @brief    This function returns the mediaType
 *
 * @note    This function is synchronous.
 * @note    This function is used specially by the music manager project
 *
 * @param    pContext        (IN)    Execution Context
 * @param    pMediaType        (OUT)    MediaType pointer
 *
 * @return    M4NO_ERROR            No error
 * @return    M4ERR_PARAMETER        At least one parameter is NULL. (in DEBUG only)
 ************************************************************************
*/
M4OSA_ERR M4MDP_getMediaType(M4MDP_Context pContext,M4READER_MediaType* pMediaType);

/******************************************************************************
* @brief        returns mediaType found in a file
* @note
* @param        pFileDescriptor (IN) : pointer to file descriptor
* @param        pFileFunction (IN)   : pointer to file function
* @param        pMediaType (OUT)     : mediaType if found
* @return       M4NO_ERROR / M4ERR_ALLOC
******************************************************************************/
M4OSA_ERR M4MDP_getMediaTypeFromFile(M4OSA_Void *pFileDescriptor,
                                       M4OSA_FileReadPointer *pFileFunction,
                                       M4READER_MediaType *pMediaType);

/******************************************************************************
* @brief        return media type by extension and content detections
* @note
* @param        pFileDescriptor (IN) : pointer to file descriptor
* @param        dataBuffer (IN)  : memory buffer
* @param        bufferSize (IN)  : buffer size
* @param        pMediaType (OUT) : mediaType if found
* @return       M4NO_ERROR / M4ERR_ALLOC
******************************************************************************/
M4OSA_ERR    M4MDP_getMediaTypeFromExtensionAndContent(M4OSA_Void *pFileDescriptor,
                                                        M4OSA_UInt8 *dataBuffer,
                                                        M4OSA_UInt32 bufferSize,
                                                        M4READER_MediaType *pMediaType);

/******************************************************************************
* @brief        return media type by content detection
* @note
* @param        dataBuffer (IN)  : memory buffer
* @param        bufferSize (IN)  : buffer size
* @param        pMediaType (OUT) : mediaType if found
* @return       M4NO_ERROR / M4ERR_ALLOC
******************************************************************************/
M4OSA_ERR    M4MDP_getMediaTypeFromContent(M4OSA_UInt8 *dataBuffer, M4OSA_UInt32 bufferSize,
                                             M4READER_MediaType *pMediaType);

/**
 ************************************************************************
 * @brief    The function parses the buffer pAsfBuffer, extracts metadata,
 *            allocates memory for pMetaData and fills in.
 *
 * @note    pAsfBuffer owns the application (caller).
 *            The application free pAsfBuffer and pMetaData
 *
 * @note    This function is synchronous.
 *
 * @param    pAsfBuffer            (IN)    input buffer
 * @param    pMetaData            (OUT)    Pointer to the metadata structure
 *
 * @return    M4NO_ERROR                        No error
 * @return    M4ERR_PARAMETER                    pContext or pAsfBuffer is NULL. (in DEBUG only)
 * @return    M4ERR_ALLOC                        There is no more memory available
 * @return    M4WAR_READER_NO_METADATA        The M4READER_Buffer doesn't contain metadata
 * @return    M4ERR_UNSUPPORTED_MEDIA_TYPE    The input file is not recognized
 ************************************************************************
*/
M4OSA_ERR M4MDP_parseASFContentDesc(M4READER_Buffer* pAsfBuffer, M4_MetaDataFields *pMetaData);


/**
 ************************************************************************
 * @brief    The function allocates memory for pMetaData and copies its
 *            pAssetFields fields
 *
 * @note    The application which calls M4MDP_parse3GppAssetField MUST free pMetaData.
 *
 * @note    This function is synchronous.
 *
 * @param    pAssetFields    (IN)    Asset fields structure filled by the 3gpp reader
 * @param    pMetaData        (OUT)    Metadata structure to be filled in
 *
 * @return    M4NO_ERROR                        No error
 * @return    M4ERR_PARAMETER                    pContext or pAssetFields is NULL. (in DEBUG only)
 * @return    M4ERR_ALLOC                        There is no more memory available
 * @return    M4ERR_UNSUPPORTED_MEDIA_TYPE    The input file is not recognized
 ************************************************************************
*/
M4OSA_ERR M4MDP_parse3GppAssetField(M4_MetaDataFields* pAssetFields, M4_MetaDataFields *pMetaData);


/**
 ************************************************************************
 * @brief    The function allocates memory for pMetaData and copies its
 *            pExifFields fields
 *
 * @note    The application which calls M4MDP_parseExifField MUST free pMetaData.
 *
 * @note    This function is synchronous.
 *
 * @param    pExifFields    (IN)    Exif fields structure filled by the exif reader
 * @param    pMetaData    (OUT)    Metadata structure to be filled in
 *
 * @return    M4NO_ERROR                        No error
 * @return    M4ERR_PARAMETER                    pContext or pAssetFields is NULL. (in DEBUG only)
 * @return    M4ERR_ALLOC                        There is no more memory available
 * @return    M4ERR_UNSUPPORTED_MEDIA_TYPE    The input file is not recognized
 ************************************************************************
*/
M4OSA_ERR M4MDP_parseExifField(M4_MetaDataFields *pExifFields, M4_MetaDataFields *pMetaData);


/**
 ************************************************************************
 * @brief    The function allocates and fills the pMetaDataStruct by parsing
 *            a buffer
 *
 * @note    pMetaDataStruct owns the application (caller).
 *            It is the responsibility of the application (caller) to free it
 *
 * @note    This function is synchronous.
 *
 * @param        pBuffer            (IN)    input buffer
 * @param        mediaType        (IN)    media type of the buffer
 * @param        pMetaDataStruct    (OUT)    Pointer to an array of metadata
 * @param        pSize            (OUT)    pMetaDataStruct size
 *
 * @return    M4NO_ERROR                    No error
 * @return    M4ERR_PARAMETER                pContext or pBuffer or pMetaDataStruct is NULL.
 *                                          (in DEBUG only)
 * @return    M4ERR_ALLOC                    There is no more memory available
 * @return    M4ERR_UNSUPPORTED_MEDIA_TYPE The media type is not supported
 * @return    M4WAR_READER_NO_METADATA    No metadata detected
 ************************************************************************
*/
M4OSA_ERR M4MDP_getMetaDataFromBuffer(M4_MetadataBuffer*    pBuffer,
                                      M4READER_MediaType    mediaType,
                                      M4_MetaDataFields**    pMetaDataStruct,
                                      M4OSA_UInt32*            pSize);

/**
 ************************************************************************
 * @brief    The function initializes the metadata  structure
 *
 * @param    pMetadata        (OUT) Pointer to M4_MetaDataFields structure
 *
 * @return    M4NO_ERROR            No error
 * @return    M4ERR_PARAMETER        pContext or pMetadata is NULL. (in DEBUG only)
 ************************************************************************
*/
M4OSA_ERR M4MDP_InitMetaDataFields(M4_MetaDataFields *pMetaDataTab);

/**
 ************************************************************************
 * @brief    The function frees the metadata  structure
 *
 * @param    pMetadata        (IN) Pointer to M4_MetaDataFields structure
 *
 * @return    M4NO_ERROR            No error
 * @return    M4ERR_PARAMETER        pContext or pMetadata is NULL. (in DEBUG only)
 ************************************************************************
*/
M4OSA_ERR M4MDP_FreeMetaDataFields(M4_MetaDataFields *pMetaDataTab);

/******************************************************************************
* @brief        returns mediaType found in a file
* @note
* @param        pContext (IN) : pointer to file descriptor
* @param        pFileDescriptor (IN) : pointer to file descriptor
* @param        pFileFunction (IN)   : pointer to file function
* @param        pMediaType (OUT)     : mediaType if found
* @return       M4NO_ERROR / M4ERR_ALLOC
******************************************************************************/
M4OSA_ERR M4MDP_getMediaTypeFromFileExtended(    M4MDP_Context pContext,
                                                M4OSA_Void *pFileDescriptor,
                                                M4OSA_FileReadPointer *pFileFunction,
                                                M4READER_MediaType *pMediaType);

/**
 ************************************************************************
 * @brief    The function to get file size
 *
 * @param    pContext        (IN) Pointer to M4MDP Context structure
 * @param    pSize            (OUT)Pointer to file size
 *
 * @return    M4NO_ERROR            No error
 * @return    M4ERR_PARAMETER        pContext or pMetadata is NULL. (in DEBUG only)
 ************************************************************************
*/
M4OSA_ERR M4MDP_getMetaDataFileSize(M4MDP_Context pContext, M4OSA_UInt32 *pSize);

#ifdef __cplusplus
}
#endif

#endif /* __M4MDP_API_H__ */
