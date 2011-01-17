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
 * @file         M4OSA_FileCache.c
 *
 * @brief        Osal File Reader and Writer with cache
 * @note         This file implements functions to manipulate
 *               filesystem access with intermediate buffers used to
 *               read and to write.
 *************************************************************************
*/

/**
 *************************************************************************
 * File cache buffers parameters (size, number of buffers, etc)
 *************************************************************************
*/
#define M4OSA_CACHEBUFFER_SIZE    (8*1024)
#define M4OSA_CACHEBUFFER_NB    6
#define M4OSA_CACHEBUFFER_NONE    -1
#define M4OSA_CACHEBUFFER_ALL    -2
#define M4OSA_EOF               -1

/** Strategy used by Osal File Cache to flush the buffers to disk.
Depending on the service, the strategy will have more or less success */
//#define BUFFER_SELECT_INITIAL        /** Initial implementation of Osal File Reader optim */
//#define BUFFER_SELECT_WITH_TIME    /** To flush in priority the buffers which have not been used for a certain time */
//#define BUFFER_SELECT_WITH_SPACE    /** To flush in priority the buffers which have not been used a lot of times */
#define BUFFER_SELECT_WITH_POS    /** To flush in priority the buffers which have the smallest position on the file */

/* to measure success of cache operations */
//#define FILECACHE_STATS

/* For performance measure */
//#define M4OSA_FILE_CACHE_TIME_MEAS

/***  To debug */
//#define NO_STRATEGY
//#define BUFFER_DISPLAY

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
#include "M4OSA_clock.h"

typedef enum
{
    fileOpentime,
    fileClosetime,
    fileReadDatatime,
    fileWriteDatatime,
    fileSeektime,
    fileGetOptiontime,
    fileSetOptiontime,
    fileExternalFlushtime,
    enum_size    /* for enum size */
} M4OSA_filetimeType;

typedef    M4OSA_Time TabFiletime[enum_size+1];

void M4OSA_FileCache_initTimeMeas(M4OSA_Context pContext);
void M4OSA_FileCache_displayTimeMeas(M4OSA_Context pContext);

#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

/* ANSI C*/
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
/* End: ANSI C includes */

#include "M4OSA_FileCommon.h"
#include "M4OSA_FileReader.h"
#include "M4OSA_FileWriter.h"

#include "M4OSA_FileCache.h"

#include "M4OSA_Memory.h"
#include "M4OSA_Debug.h"
#include "M4OSA_CharStar.h"
#include "M4OSA_Mutex.h"


#define LOCK \
    M4OSA_mutexLock(apContext->m_mutex, M4OSA_WAIT_FOREVER);

#define UNLOCK \
    M4OSA_mutexUnlock(apContext->m_mutex);

typedef struct
{
    M4OSA_Void*                FileDesc;
} M4OSA_FileSystem_FFS_t_cache;

typedef struct
{
    M4OSA_Void*        (*pFctPtr_Open)( M4OSA_Void* fd,
                                       M4OSA_UInt32 FileModeAccess,
                                       M4OSA_UInt16* errno_ffs );
    M4OSA_FilePosition (*pFctPtr_Read)( M4OSA_Void* fd,
                                       M4OSA_UInt8* data,
                                       M4OSA_FilePosition size,
                                       M4OSA_UInt16* errno_ffs );
    M4OSA_FilePosition (*pFctPtr_Write)( M4OSA_Void* fd,
                                         M4OSA_UInt8* data,
                                         M4OSA_FilePosition size,
                                         M4OSA_UInt16* errno_ffs );
    M4OSA_FilePosition (*pFctPtr_Seek)( M4OSA_Void* fd,
                                        M4OSA_FilePosition pos,
                                        M4OSA_FileSeekAccessMode mode,
                                        M4OSA_UInt16* errno_ffs );
    M4OSA_FilePosition (*pFctPtr_Tell)( M4OSA_Void* fd,
                                        M4OSA_UInt16* errno_ffs );
    M4OSA_Int32        (*pFctPtr_Close)( M4OSA_Void* fd,
                                         M4OSA_UInt16* errno_ffs );
    M4OSA_Void         (*pFctPtr_AccessType)( M4OSA_UInt32 FileModeAccess_In,
                                             M4OSA_Void* FileModeAccess_Out );

} M4OSA_FileSystem_FctPtr_cache;

M4OSA_Void* M4OSA_FileSystem_FFS_Open_cache( M4OSA_Void* pFileDescriptor,
                                             M4OSA_UInt32 FileModeAccess,
                                             M4OSA_UInt16* errno_ffs );
M4OSA_Int32 M4OSA_FileSystem_FFS_Close_cache( M4OSA_Void* pContext,
                                              M4OSA_UInt16* errno_ffs );
M4OSA_FilePosition M4OSA_FileSystem_FFS_Read_cache( M4OSA_Void* pContext,
                                                    M4OSA_UInt8* data,
                                                    M4OSA_FilePosition size,
                                                    M4OSA_UInt16* errno_ffs );
M4OSA_FilePosition M4OSA_FileSystem_FFS_Write_cache( M4OSA_Void* pContext,
                                                     M4OSA_UInt8* data,
                                                     M4OSA_FilePosition size,
                                                     M4OSA_UInt16* errno_ );
M4OSA_Int32 M4OSA_FileSystem_FFS_Seek_cache( M4OSA_Void* pContext,
                                             M4OSA_FilePosition pos,
                                             M4OSA_FileSeekAccessMode mode,
                                             M4OSA_UInt16* errno_ffs );
M4OSA_FilePosition M4OSA_FileSystem_FFS_Tell_cache( M4OSA_Void* pContext,
                                                    M4OSA_UInt16* errno_ffs );

M4OSA_ERR M4OSA_fileOpen_cache_internal(M4OSA_Context* pContext,
                                        M4OSA_Void* pFileDescriptor,
                                        M4OSA_UInt32 FileModeAccess,
                                        M4OSA_FileSystem_FctPtr_cache *FS);

/*
------------------User--------------------
                   ^
                   |
--------    --------    ----------
|Filled|    |Copied|    |Modified|
--------    --------    ----------
  ^
  |
------------------Disk--------------------

Atomic states for a buffer:

0x00    initialized or flushed (When it is initialized again, it is flushed if necessary)
0x01    Filled from disk
0x03    Filled and Copied to user

0x80    Modified and newly created (does not exist on disk) => must be flushed
0x83    Modified after having been read from disk => must be flushed

*/

typedef enum
{
    M4OSA_kInitialized = 0,
    M4OSA_kFilled = 0x1,
    M4OSA_kCopied = 0x2,
    M4OSA_kModified = 0x80
} M4OSA_FileCacheStateAtomic;


/**
 ******************************************************************************
 * structure    M4OSA_FileCache_Buffer
 * @brief       This structure defines the File Buffers context (private)
 ******************************************************************************
*/
typedef struct
{
    M4OSA_MemAddr8      data;        /**< buffer data */
    M4OSA_FilePosition  size;        /**< size of the buffer */
    M4OSA_FilePosition  filepos;    /**< position in the file of the buffer's first octet */
    M4OSA_FilePosition  remain;        /**< data amount not already copied from buffer */

    M4OSA_UInt32        nbFillSinceLastAcess;    /**< To know since how many time we didn't use this buffer. to detect  dead buffers */

    M4OSA_UInt32        nbAccessed;            /**< nb of times the buffer has been accessed without being reinitialized */
    M4OSA_Time            timeAccessed;         /**< last time at which the buffer has been accessed without being reinitialized */

    M4OSA_UInt8            state;
} M4OSA_FileCache_Buffer;

/**
 ******************************************************************************
 * structure    M4OSA_FileCache_Context
 * @brief       This structure defines the File context (private)
 * @note        This structure is used for all File calls to store the context
 ******************************************************************************
*/
typedef struct
{
    M4OSA_Bool          IsOpened;               /**< Micro state machine */
    M4OSA_FileAttribute FileAttribute;          /**< Opening mode */
    M4OSA_FilePosition     readFilePos;            /**< Effective position of the file pointer */
    M4OSA_FilePosition     absolutePos;            /**< Virtual position for next reading */
    M4OSA_FilePosition     absoluteWritePos;        /**< Virtual position for next writing */
    M4OSA_FilePosition     fileSize;                /**< Size of the file */
    M4OSA_FilePosition     virtualFileSize;        /**< Size of the file */

    M4OSA_FileCache_Buffer buffer[M4OSA_CACHEBUFFER_NB];  /**< buffers */

    M4OSA_Void*             aFileDesc;          /**< File descriptor */
    M4OSA_FileSystem_FctPtr_cache *FS;                /**< Filesystem interface */

#ifdef FILECACHE_STATS
    M4OSA_UInt32        cacheSuccessRead;
    M4OSA_UInt32        cacheSuccessWrite;
    M4OSA_UInt32        nbReadCache;
    M4OSA_UInt32        nbWriteCache;

    M4OSA_UInt32        nbReadFFS;
    M4OSA_UInt32        nbWriteFFS;
#endif /* FILECACHE_STATS */
    M4OSA_Context        m_mutex;

    M4OSA_Time            chrono;
    M4OSA_Char            m_filename[256];

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    TabFiletime            gMyPerfFileTab;
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

} M4OSA_FileCache_Context;


#define M4ERR_CHECK_NULL_RETURN_VALUE(retval, pointer) if ((pointer) == M4OSA_NULL) return (retval);

/* used to detect dead buffers */
#define MAX_FILLS_SINCE_LAST_ACCESS    M4OSA_CACHEBUFFER_NB*2


/* __________________________________________________________ */
/*|                                                          |*/
/*|        Quick Sort function    (private)                     |*/
/*|__________________________________________________________|*/

M4OSA_Void M4OSA_FileCache_internalQuicksort(M4OSA_Int32* const table,
                               const M4OSA_Int32 first , const M4OSA_Int32 last)
 {
    M4OSA_Int32 startIndex;
    M4OSA_Int32 endIndex;
    M4OSA_Int32 begin;
    M4OSA_Int32 end;
    M4OSA_Int32 pivot;
    M4OSA_Int32 temp;
    M4OSA_Int32 index=0;
    M4OSA_Int32 size=0;
    M4OSA_Int32 capacity = M4OSA_CACHEBUFFER_NB * 5;
    //allocation of the fifo
    M4OSA_Int32* queue = M4OSA_NULL;
    M4OSA_Int32* cursor;
    M4OSA_Int32* indexc;

    queue = (M4OSA_Int32*)M4OSA_malloc(capacity*sizeof(M4OSA_Int32), 0,
                                   (M4OSA_Char*) "quicksort FIFO of FileCache");

    if(queue == M4OSA_NULL)
        return;
    cursor = queue;
    indexc = queue;
    *(cursor++) = first; //remember the array first element
    *(cursor++) = last;    //remember the array end
    index = 0;
    size = 2;
    do
    {
        startIndex   = *(indexc++);
        endIndex    = *(indexc++);
        index+=2;
        if(startIndex < endIndex)
        {
            begin    = startIndex;
            end        = endIndex;
            pivot    = table[endIndex];
            do
            {
                while ( (begin < endIndex) && (table[begin]<=pivot) )
                    begin++;
                while ( (end > begin) && (table[end]>=pivot) )
                    end--;
                if (begin < end)
                {
                    temp            = table[end];
                    table[end]        = table[begin];
                    table[begin]    = temp;
                }
            }while(begin < end);

            temp            = table[endIndex];
            table[endIndex]        = table[begin];
            table[begin]    = temp;
            *(cursor++) = startIndex;
            *(cursor++) = begin-1;
            *(cursor++) = begin+1;
            *(cursor++) =  endIndex;
            size+=4;
            if(size==capacity)
            {
                M4OSA_TRACE1_0("Overflow in quickSort. increase capacity size");
                return;
            }
        }
    }
    while(index<size);
    cursor = NULL;
    indexc = NULL;
    M4OSA_free(queue);
}

M4OSA_Void M4OSA_FileCache_internalQuicksort64(M4OSA_Int64* const table,
                               const M4OSA_Int32 first , const M4OSA_Int32 last)
 {
    M4OSA_Int32 startIndex;
    M4OSA_Int32 endIndex;
    M4OSA_Int32 begin;
    M4OSA_Int32 end;
    M4OSA_Int64 pivot;
    M4OSA_Int64 temp;
    M4OSA_Int32 index=0;
    M4OSA_Int32 size=0;
    M4OSA_Int32 capacity = M4OSA_CACHEBUFFER_NB * 5;
    //allocation of the fifo
    M4OSA_Int32* queue = M4OSA_NULL;
    M4OSA_Int32* cursor;
    M4OSA_Int32* indexc;

    queue = (M4OSA_Int32*)M4OSA_malloc(capacity*sizeof(M4OSA_Int32), 0,
                                   (M4OSA_Char*) "quicksort FIFO of FileCache");

    if(queue == M4OSA_NULL)
        return;
    cursor = queue;
    indexc = queue;
    *(cursor++) = first; //remember the array first element
    *(cursor++) = last;    //remember the array end
    index = 0;
    size = 2;
    do
    {
        startIndex   = *(indexc++);
        endIndex    = *(indexc++);
        index+=2;
        if(startIndex < endIndex)
        {
            begin    = startIndex;
            end        = endIndex;
            pivot    = table[endIndex];
            do
            {
                while ( (begin < endIndex) && (table[begin]<=pivot) )
                    begin++;
                while ( (end > begin) && (table[end]>=pivot) )
                    end--;
                if (begin < end)
                {
                    temp            = table[end];
                    table[end]        = table[begin];
                    table[begin]    = temp;
                }
            }while(begin < end);

            temp            = table[endIndex];
            table[endIndex]        = table[begin];
            table[begin]    = temp;
            *(cursor++) = startIndex;
            *(cursor++) = begin-1;
            *(cursor++) = begin+1;
            *(cursor++) =  endIndex;
            size+=4;
            if(size==capacity)
            {
                M4OSA_TRACE1_0("Overflow in quickSort. increase capacity size");
                return;
            }
        }
    }
    while(index<size);
    cursor = NULL;
    indexc = NULL;
    M4OSA_free(queue);
}

/* Sorts an array of size size */
M4OSA_Void M4OSA_FileCache_QS_quickSort (M4OSA_FilePosition array[],
                                                              M4OSA_UInt32 size)
{
    if (size==1 || size==0)
    {
        M4OSA_TRACE3_0("Sort not necessary");
        return;
    }

    M4OSA_FileCache_internalQuicksort(array,0,size-1);
}

M4OSA_Void M4OSA_FileCache_QS_quickSort64 (M4OSA_Time array[], M4OSA_UInt32 size)
{
    if (size==1 || size==0)
    {
        M4OSA_TRACE3_0("Sort not necessary");
        return;
    }

    M4OSA_FileCache_internalQuicksort64(array,0,size-1);
}

/* __________________________________________________________ */
/*|                                                          |*/
/*|   Buffer handling functions for RW access  (private)     |*/
/*|__________________________________________________________|*/

/**************************************************************/
M4OSA_ERR M4OSA_FileCache_BuffersInit(M4OSA_FileCache_Context* apContext)
/**************************************************************/
{
    M4OSA_UInt8 i;

    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        apContext->buffer[i].state = M4OSA_kInitialized;
        M4OSA_memset((M4OSA_MemAddr8)&(apContext->buffer[i]),
                                            sizeof(M4OSA_FileCache_Buffer) , 0);
    }

    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        apContext->buffer[i].data = (M4OSA_MemAddr8) M4OSA_malloc(M4OSA_CACHEBUFFER_SIZE,
                  M4OSA_FILE_READER, (M4OSA_Char*)"M4OSA_FileCache_BufferInit");
        M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_ALLOC, apContext->buffer[i].data);
        apContext->buffer[i].filepos = M4OSA_EOF;

    }

    return M4NO_ERROR;
}

/**************************************************************/
M4OSA_Void M4OSA_FileCache_BuffersFree(M4OSA_FileCache_Context* apContext)
/**************************************************************/
{
    M4OSA_UInt8 i;

    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if(apContext->buffer[i].data != M4OSA_NULL)
        {
            M4OSA_free((M4OSA_MemAddr32)apContext->buffer[i].data);
            apContext->buffer[i].data = M4OSA_NULL;
        }
    }
}

#ifdef BUFFER_DISPLAY
M4OSA_Void M4OSA_FileCache_BufferDisplay(M4OSA_FileCache_Context* apContext)
{
    M4OSA_UInt32 i;

    M4OSA_TRACE1_0("------ Buffers display ");
    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        M4OSA_TRACE1_5("------ Buf%d : FilePos=%d state=0x%x nbAccessed=%d -- timeAccessed=%d",
                                                i, apContext->buffer[i].filepos,
                                                apContext->buffer[i].state,
                                                apContext->buffer[i].nbAccessed,
                                                apContext->buffer[i].timeAccessed);
    }
    M4OSA_TRACE1_0("---------------------- ");
}
#endif


/* reads from an existing buffer (number i) at absolute position pos and fills pData. it reads maximum one bloc */
/**************************************************************/
M4OSA_FilePosition M4OSA_FileCache_BufferCopy(M4OSA_FileCache_Context* apContext,
                                              M4OSA_Int8 i, M4OSA_FilePosition pos,
                                              M4OSA_FilePosition size,
                                              M4OSA_MemAddr8 pData)
/**************************************************************/
{
    M4OSA_FilePosition copysize;
    M4OSA_FilePosition offset;

    M4OSA_TRACE3_2("BufferCopy of %d, pos=%d", i, pos);

    if(apContext->buffer[i].size == M4OSA_EOF) return M4OSA_EOF;

    /* verification pos is inside buffer i*/
    if(   (pos < apContext->buffer[i].filepos)
       || (pos > (apContext->buffer[i].filepos + apContext->buffer[i].size - 1)) )
    {
        return 0; /* nothing copied */
    }

    offset = pos - apContext->buffer[i].filepos;    /* offset to read from in the buffer */

    copysize = apContext->buffer[i].size - offset;    /* buffer size - offset, it is the maximum we can read from a buffer */
    copysize = (size < copysize) ? size : copysize; /* adjust in case user wants to read less than the data available in the buffer (copysize)
                                                        in that case, take the min(copysize, size)*/

    M4OSA_memcpy(pData, apContext->buffer[i].data + offset, copysize);

    apContext->buffer[i].remain -= copysize;
    apContext->buffer[i].nbFillSinceLastAcess = 0;


    /* it is a read access */
    apContext->buffer[i].nbAccessed++;
    apContext->buffer[i].timeAccessed = apContext->chrono;
    apContext->chrono++;


    apContext->buffer[i].state |= M4OSA_kCopied;

    return copysize;
}

/* writes on cache. at absolute position pos and writes pData to the buffer. it writes maximum one bloc */
/**************************************************************/
M4OSA_FilePosition M4OSA_FileCache_BufferModifyContent(M4OSA_FileCache_Context* apContext,
                                                       M4OSA_Int8 i, M4OSA_FilePosition pos,
                                                       M4OSA_FilePosition size,
                                                       M4OSA_MemAddr8 pData)
/**************************************************************/
{
    M4OSA_FilePosition copysize;
    M4OSA_FilePosition offset, gridPos;

    M4OSA_TRACE3_2("BufferModify of %d, pos=%d", i, pos);

    /* Relocate to absolute postion if necessary */
    gridPos = (pos / M4OSA_CACHEBUFFER_SIZE) * M4OSA_CACHEBUFFER_SIZE;

    apContext->buffer[i].filepos = gridPos;

    /* in case of an existing block (not at eof) */
    if    (apContext->buffer[i].size != 0)
    {
        /* test if we are already inside this buffer */
        if (   (pos < apContext->buffer[i].filepos)
           || (pos > (apContext->buffer[i].filepos + M4OSA_CACHEBUFFER_SIZE)) )
        {
            M4OSA_TRACE1_0("BufferModify ERR nothing copied, should never happen");
            return 0; /* nothing copied */
        }
    }

    offset = pos - apContext->buffer[i].filepos;    /* offset to write to, in the buffer */

    /* buffer size - offset, it is the maximum we can write into a buffer */
    copysize = M4OSA_CACHEBUFFER_SIZE - offset;
    /* adjust in case user wants to write less than the data available in the buffer (copysize) in that case, take the min(copysize, size)*/
    copysize = (copysize < size) ? copysize : size;

    M4OSA_memcpy(apContext->buffer[i].data + offset, pData, copysize);

    /* update buffer size if it is a new buffer or expanded one*/
    if (apContext->buffer[i].size < copysize+offset)
    {
        apContext->buffer[i].size = copysize+offset;
    }

    apContext->buffer[i].remain = M4OSA_CACHEBUFFER_SIZE - apContext->buffer[i].size; /* not temporary */

    /* mark this buffer as modified */
    apContext->buffer[i].state |= M4OSA_kModified;

    apContext->buffer[i].nbFillSinceLastAcess = 0;
    apContext->buffer[i].nbAccessed++;
    apContext->buffer[i].timeAccessed = apContext->chrono;
    apContext->chrono++;

    return copysize;
}


/* writes in a buffer (number i) with the data read from disk*/
/**************************************************************/
M4OSA_ERR M4OSA_FileCache_BufferFill(M4OSA_FileCache_Context* apContext,
                                     M4OSA_Int8 i, M4OSA_FilePosition pos)
/**************************************************************/
{
    M4OSA_FilePosition gridPos;
    M4OSA_FilePosition diff;
    M4OSA_FilePosition size;
    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_Int32 ret_val;
    M4OSA_UInt16 errReturned;

    M4OSA_TRACE3_2("BufferFill  i = %d  pos = %d", i, pos);

    /* Avoid cycling statement because of EOF */
    if(pos > apContext->virtualFileSize)
            return M4WAR_NO_MORE_AU;

    /* Relocate to absolute postion if necessary */
    gridPos = (pos / M4OSA_CACHEBUFFER_SIZE) * M4OSA_CACHEBUFFER_SIZE;

    /* diff is how much shall we fs_seek from current position to reach gridPos*/
    diff = gridPos - apContext->readFilePos;

    /* on some filesystems it is necessary to do a seek between an ffs read and ffs write even if it is same pos */
        ret_val = apContext->FS->pFctPtr_Seek(apContext->aFileDesc,
                                    diff, M4OSA_kFileSeekCurrent, &errReturned);
        apContext->readFilePos = gridPos;

        if(ret_val != 0)
        {
            err = M4OSA_ERR_CREATE(M4_ERR, M4OSA_FILE_READER, errReturned);
            M4OSA_TRACE2_1("M4OSA_FileCache_BufferFill ERR1 = 0x%x", err);
            return err;
        }
    /* end. on some */

/* the buffer will be reused to be filled with another filepos so reinit counters of access */
    if (apContext->buffer[i].filepos  != gridPos)
    {
        apContext->buffer[i].nbAccessed = 0;
        apContext->buffer[i].timeAccessed = 0;
    }

    /* stores the information relative to this buffer */
    apContext->buffer[i].filepos = gridPos;

    /* Read Data */
    size = apContext->FS->pFctPtr_Read(apContext->aFileDesc,
                                       (M4OSA_UInt8*)apContext->buffer[i].data,
                                        M4OSA_CACHEBUFFER_SIZE, &errReturned);

#ifdef FILECACHE_STATS
    apContext->nbReadFFS++;
#endif /* FILECACHE_STATS */

    if(size == -1)
    {
        apContext->buffer[i].size = M4OSA_EOF;
        apContext->buffer[i].remain = 0;

        err = M4OSA_ERR_CREATE(M4_ERR, M4OSA_FILE_READER, errReturned);
        M4OSA_TRACE2_1("M4OSA_FileCache_BufferFill ERR2 = 0x%x", err);
        return err;
    }

    apContext->buffer[i].size = size;
    apContext->buffer[i].remain = size;
    apContext->buffer[i].nbFillSinceLastAcess = 0;


    /* Retrieve current position */
    apContext->readFilePos = apContext->FS->pFctPtr_Tell(apContext->aFileDesc,
                                                                  &errReturned);

    if(   (apContext->buffer[i].size >= 0)
       && (apContext->buffer[i].size < M4OSA_CACHEBUFFER_SIZE) )
    {
        err = M4WAR_NO_DATA_YET;
        M4OSA_TRACE2_1("M4OSA_FileCache_BufferFill ERR3 = 0x%x", err);
        return err;
    }

    /* mark this buffer as modified */
    apContext->buffer[i].state |= M4OSA_kFilled;

    /* it is a write access */
    apContext->buffer[i].nbAccessed++;
    apContext->buffer[i].timeAccessed = apContext->chrono;
    apContext->chrono++;

    /* Return without error */
    return M4NO_ERROR;
}

/*  Reinitializes a buffer for filling it for data at end of file. fileposition is given */
/**************************************************************/
M4OSA_ERR M4OSA_FileCache_BufferReinitialize(M4OSA_FileCache_Context* apContext,
                                           M4OSA_Int8 i, M4OSA_FilePosition pos)
/**************************************************************/
{
    M4OSA_FilePosition gridPos;
    M4OSA_ERR err = M4NO_ERROR;

    M4OSA_MemAddr8 saveDataAddress;

    M4OSA_TRACE3_2("BufferReinitialize i = %d  pos = %d", i, pos);

    /* Relocate to absolute postion if necessary */
    if (pos != -1)
        gridPos = (pos / M4OSA_CACHEBUFFER_SIZE) * M4OSA_CACHEBUFFER_SIZE;
    else
        gridPos = -1;

    /* RAZ the buffer, only "data address" stays*/
    saveDataAddress = apContext->buffer[i].data;

    M4OSA_memset((M4OSA_MemAddr8)&(apContext->buffer[i]),
                                            sizeof(M4OSA_FileCache_Buffer) , 0);

    /* put again the precious "data address" previously allocated */
    apContext->buffer[i].data = saveDataAddress;

    /* initializations already done in the memset: */
    /* mark this buffer as initialized*/
    apContext->buffer[i].state= M4OSA_kInitialized;
    apContext->buffer[i].nbAccessed = 0;
    apContext->buffer[i].timeAccessed = 0;

    /* Relocate to absolute postion if necessary */
    apContext->buffer[i].filepos = gridPos;

    /* Return without error */
    return M4NO_ERROR;
}


/* flushes a buffer (number i) to the disk*/
/**************************************************************/
M4OSA_ERR M4OSA_FileCache_BufferFlush(M4OSA_FileCache_Context* apContext,
                                                                  M4OSA_UInt8 i)
/**************************************************************/
{
    M4OSA_FilePosition gridPos, pos;
    M4OSA_FilePosition diff;
    M4OSA_FilePosition size;
    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_Int32 ret_val;
    M4OSA_UInt16 errReturned;

    M4OSA_TRACE3_2("BufferFlush of buffer i=%d its pos=%d", i,
                                                  apContext->buffer[i].filepos);

    pos = apContext->buffer[i].filepos;

    /* Relocate to absolute postion if necessary */
    gridPos = (pos / M4OSA_CACHEBUFFER_SIZE) * M4OSA_CACHEBUFFER_SIZE;

    /* diff is how much shall we fs_seek from current position to reach gridPos*/
    diff = gridPos - apContext->readFilePos;

    if (pos > apContext->fileSize)
    {
        M4OSA_TRACE1_2("M4OSA_FileCache_BufferFlush: Error! Attempt to seek at pos=%d, whilst fileSize=%d ",
                                                      pos, apContext->fileSize);
        return M4WAR_NO_MORE_AU;
    }

    /* on some filesystems it is necessary to do a seek between an ffs read and ffs write even if it is same pos */
        ret_val = apContext->FS->pFctPtr_Seek(apContext->aFileDesc, diff,
                                          M4OSA_kFileSeekCurrent, &errReturned);
        apContext->readFilePos = gridPos;

        if(ret_val != 0)
        {
            err = M4OSA_ERR_CREATE(M4_ERR, M4OSA_FILE_READER, errReturned);
            M4OSA_TRACE1_1("M4OSA_FileCache_BufferFill ERR1 = 0x%x", err);
            return err;
        }
    /* end: on some filesystems*/

    /* update the read file pos after the seek */
    apContext->readFilePos = apContext->buffer[i].filepos;

    /* Write Data */
    size = apContext->FS->pFctPtr_Write(apContext->aFileDesc,
                                        (M4OSA_UInt8*)apContext->buffer[i].data,
                                        apContext->buffer[i].size, &errReturned);
#ifdef FILECACHE_STATS
    apContext->nbWriteFFS++;
#endif /* FILECACHE_STATS */
     /* verify if all data requested to be written, has been written */
    if(size < apContext->buffer[i].size)
    {
        apContext->buffer[i].size = M4OSA_EOF;
        apContext->buffer[i].remain = 0;

        err = M4OSA_ERR_CREATE(M4_ERR, M4OSA_FILE_READER, errReturned);
        M4OSA_TRACE1_1("M4OSA_FileCache_BufferFlush ERR2 = 0x%x", err);
        return err;
    }

    /* Retrieve current position */
    apContext->readFilePos = apContext->FS->pFctPtr_Tell(apContext->aFileDesc,
                                                                  &errReturned);

    apContext->fileSize = (apContext->readFilePos > apContext->fileSize) ? apContext->readFilePos : apContext->fileSize;

    /* mark this buffer as not modified */
    apContext->buffer[i].state &= ~(M4OSA_kModified);

    /* Return without error */
    return M4NO_ERROR;
}

/* flushes all modified buffers until the position of buffer i
if index is M4OSA_CACHEBUFFER_ALL then flush all*/
/**************************************************************/
M4OSA_ERR M4OSA_FileCache_BuffersFlushUntil(M4OSA_FileCache_Context* apContext,
                                                               M4OSA_Int8 index)
/**************************************************************/
{
    M4OSA_UInt8 i, j, howManyToFlush;
    M4OSA_FilePosition bufPos[M4OSA_CACHEBUFFER_NB];
    M4OSA_Bool flushed = M4OSA_FALSE;
    M4OSA_ERR err = M4NO_ERROR;


    i=0;
    for(j=0; j<M4OSA_CACHEBUFFER_NB; j++)
    {
        if ( ((apContext->buffer[j].state & M4OSA_kModified) == M4OSA_kModified)                    )
        {
            bufPos[i] = apContext->buffer[j].filepos;
            i++;
        }
    }
    howManyToFlush = i;

    if (howManyToFlush == 0)
    {
        M4OSA_TRACE2_0("BuffersFlush : no flush needed");
        return M4NO_ERROR;
    }
    else if (howManyToFlush == 1)
    {
        goto flushing;
    }

    M4OSA_FileCache_QS_quickSort(bufPos, howManyToFlush);

flushing:
    if (index == M4OSA_CACHEBUFFER_ALL)
    {/* simply flush all buffers in order of positions */
        for(j=0; j<howManyToFlush; j++)
        {
            for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
            if (apContext->buffer[i].filepos == bufPos[j] )
            {
                M4OSA_TRACE2_2("M4OSA_FileCache_BuffersFlushUntil(1) : We Need to Flush buffer %d its pos=%d",
                                               i, apContext->buffer[i].filepos);
                err = M4OSA_FileCache_BufferFlush(apContext, i);
                if (M4NO_ERROR!= err)
                {
                    return err;
                }
                break;
            }
        }
    }
    else
    { /* there is a given index to flush until it*/
        for(j=0; j<howManyToFlush; j++)
        {
            for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
            {
                if (
                        apContext->buffer[i].filepos == bufPos[j]
                        && apContext->buffer[i].filepos <= apContext->buffer[index].filepos
                        && apContext->buffer[i].filepos >= apContext->fileSize - M4OSA_CACHEBUFFER_SIZE
                    )
                    {
                        M4OSA_TRACE2_2("M4OSA_FileCache_BuffersFlushUntil(2) : We Need to Flush buffer %d its pos=%d",
                                               i, apContext->buffer[i].filepos);
                        err = M4OSA_FileCache_BufferFlush(apContext, i);
                        if (M4NO_ERROR!= err)
                        {
                            return err;
                        }
                        if (i==index)
                        {    /* the buffer with the given index has been flushed */
                            flushed = M4OSA_TRUE;
                        }
                        break;
                    }
            }

        }

        if (M4OSA_TRUE == flushed)
        {
            err = M4NO_ERROR;
        }
        else
        {
            M4OSA_TRACE1_1("M4OSA_FileCache_BuffersFlushUntil err=0x%x", err);
            err = M4ERR_BAD_CONTEXT;
        }

    }

    return err;

}

#ifdef BUFFER_DISPLAY
M4OSA_Void M4OSA_FileCache_BufferDisplay(M4OSA_FileCache_Context* apContext)
{
    M4OSA_UInt32 i;

    M4OSA_TRACE1_0("------ Buffers display ");
    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        M4OSA_TRACE1_3("------ Buf%d : FilePos=%d state=0x%x ",
                   i, apContext->buffer[i].filepos, apContext->buffer[i].state);
#ifdef BUFFER_DATE
        M4OSA_TRACE1_2("nbAccessed=%d - nbModified =%d",
              apContext->buffer[i].nbAccessed, apContext->buffer[i].nbModified);
        M4OSA_TRACE1_2("timeAccessed=%d - timeModified =%d",
                                              apContext->buffer[i].timeAccessed,
                                               apContext->buffer[i].timeModified);
#endif /* BUFFER_DATE */
    }
    M4OSA_TRACE1_0("---------------------- ");
}
#endif


/* provides the buffer corresponding to a position pos
and with pos inside the read data into this buffer
else returns CACHE_BUFFER_NONE*/
/**************************************************************/
M4OSA_Int8 M4OSA_FileCache_BufferMatchToRead(M4OSA_FileCache_Context* apContext,
                                                         M4OSA_FilePosition pos)
/**************************************************************/
{
    M4OSA_Int8 i;

    /* Select the buffer which matches with given pos */
    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if(   (pos >= apContext->buffer[i].filepos)
            && (pos < apContext->buffer[i].filepos + apContext->buffer[i].size)
            && (apContext->buffer[i].filepos != M4OSA_EOF)                     )
        {
            M4OSA_TRACE3_1("BufferMatch returns  i = %d", i);
            return i;
        }
    }

    M4OSA_TRACE3_1("BufferMatch returns  N O N E !!!", i);
    return M4OSA_CACHEBUFFER_NONE;
}


/* provides the buffer corresponding to a position pos
and with pos inside its maximum capacity
else returns CACHE_BUFFER_NONE*/
/**************************************************************/
M4OSA_Int8 M4OSA_FileCache_BufferMatchToWrite(M4OSA_FileCache_Context* apContext,
                                                         M4OSA_FilePosition pos)
/**************************************************************/
{
    M4OSA_Int8 i;

    /* Select the buffer which matches with given pos */
    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if(   (pos >= apContext->buffer[i].filepos)
           && (pos < apContext->buffer[i].filepos + M4OSA_CACHEBUFFER_SIZE)
            && (apContext->buffer[i].filepos != M4OSA_EOF)                )
        {
            M4OSA_TRACE3_1("BufferMatch returns  i = %d", i);
            return i;
        }
    }

    M4OSA_TRACE3_1("BufferMatch returns  N O N E !!!", i);
    return M4OSA_CACHEBUFFER_NONE;
}

/* chooses a buffer by overwriting an existing one and returns i */
/**************************************************************/
M4OSA_Int8 M4OSA_FileCache_BufferSelectForWrite(M4OSA_FileCache_Context* apContext)
/**************************************************************/
{
    M4OSA_Int8 i;
    M4OSA_UInt8 selected = 0;
    M4OSA_UInt32 j, toSort;
    M4OSA_FilePosition bufPos[M4OSA_CACHEBUFFER_NB];
    M4OSA_ERR err;

    // update nbFillSinceLastAcess field
    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        apContext->buffer[i].nbFillSinceLastAcess ++;
    }
#ifdef NO_STRATEGY
    goto end_selection;
#endif

    /*********************************************/
    /* 1/ if there is still a new buffer, use it */

    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if(apContext->buffer[i].state == M4OSA_kInitialized)
        {
            selected = i;
            goto end_selection;
        }
    }

    /*********************************************/
    /* 2/ Choose a filled and copied buffer      */

    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if( ((apContext->buffer[i].state & M4OSA_kFilled) == M4OSA_kFilled)
            && ((apContext->buffer[i].state & M4OSA_kCopied) == M4OSA_kCopied)
            && ((apContext->buffer[i].state & M4OSA_kModified) != M4OSA_kModified)   /* bug fix modified */
           )
        {
            selected = i;
            goto end_selection;
        }
    }

    /****************************************************************/
    /* 3/ Choose a modified buffer with filepos>threshold and min   */
    i=0;

    /* sort the buffers by filepos and choose the min and not < threshold*/
    for(j=0; j<M4OSA_CACHEBUFFER_NB; j++)
    {
        if  (
             ((apContext->buffer[j].state & M4OSA_kModified) == M4OSA_kModified)
             && (apContext->buffer[j].filepos > -1) /* not EOF */
             )
        {
            bufPos[i] = apContext->buffer[j].filepos;
            i++;
        }
    }

    toSort = i;
    if (toSort == 0 )
    {
        selected = 0;
        goto end_selection;
    }
    else if (toSort ==1 )
    {
        goto skip_sort;
    }
    else
    {
        M4OSA_FileCache_QS_quickSort(bufPos, toSort);
    }

skip_sort:
    /* take the smallest filepos */
    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if (apContext->buffer[i].filepos == bufPos[0])
        {
            selected = i;
            goto end_selection;
        }
    }

end_selection:
    if (apContext->buffer[selected].filepos > apContext->fileSize )
    {
        /* in case it selects a modified buffer outside real file size,
        in that case, flush all buffers before this one
        unless there will be a seek outside filesize*/
        M4OSA_FileCache_BuffersFlushUntil(apContext, selected);
    }
    else if ((apContext->buffer[selected].state & M4OSA_kModified) == M4OSA_kModified )
    {
        /* in case it selects a modified buffer inside filesize, simply flush it*/
        err = M4OSA_FileCache_BufferFlush(apContext, selected);
        if (M4NO_ERROR!= err)
        {
            return M4OSA_CACHEBUFFER_NONE;
        }
    }

#ifdef NO_STRATEGY
    /* selected stays 0 */
    err = M4OSA_FileCache_BuffersFlushUntil(apContext, M4OSA_CACHEBUFFER_ALL);
    if (M4NO_ERROR!= err)
    {
        return M4OSA_FILE_CACHE_BUFFER_NONE;
    }
#endif

    M4OSA_TRACE3_1("---------- BufferSelect returns  i = %d", selected);
     return selected;
}


/* chooses a buffer by overwriting an existing one and returns i */
/**************************************************************/
M4OSA_Int8 M4OSA_FileCache_BufferSelectWithTime(M4OSA_FileCache_Context* apContext)
/**************************************************************/
{
    M4OSA_Int8 i;
    M4OSA_UInt8 selected = 0;
    M4OSA_UInt32 j, toSort;
    M4OSA_Time bufTime[M4OSA_CACHEBUFFER_NB];
    M4OSA_ERR err;

    /*********************************************/
    /* 1/ if there is still a new buffer, use it */

    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if(apContext->buffer[i].state == M4OSA_kInitialized)
        {
            selected = i;
            goto end_selection;
        }
    }

    i=0;
    /* sort all buffers with order of timeAccessed */
    for(j=0; j<M4OSA_CACHEBUFFER_NB; j++)
    {
        bufTime[i] = apContext->buffer[j].timeAccessed;
        i++;
    }

    toSort = i;
    if (toSort == 0 )
    {
        selected = 0;
        goto end_selection;
    }
    else if (toSort ==1 )
    {
        goto skip_sort;
    }
    else
    {
        M4OSA_FileCache_QS_quickSort64(bufTime, toSort);
    }

skip_sort:
    /* take the smallest timeAccessed */
    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if (apContext->buffer[i].timeAccessed == bufTime[0])
        {
            selected = i;
            goto end_selection;
        }
    }

end_selection:
    if (apContext->buffer[selected].filepos > apContext->fileSize )
    {
        /* in case it selects a modified buffer outside real file size,
        in that case, flush all buffers before this one
        unless there will be a seek outside filesize*/
        M4OSA_FileCache_BuffersFlushUntil(apContext, selected);
    }
    else if ((apContext->buffer[selected].state & M4OSA_kModified) == M4OSA_kModified )
    {
        /* in case it selects a modified buffer inside filesize, simply flush it*/
        err = M4OSA_FileCache_BufferFlush(apContext, selected);
        if (M4NO_ERROR!= err)
        {
            return M4OSA_CACHEBUFFER_NONE;
        }
    }
    M4OSA_TRACE3_1("---------- BufferSelect returns  i = %d", selected);
     return selected;
}

/* chooses a buffer by overwriting an existing one and returns i */
/**************************************************************/
M4OSA_Int8 M4OSA_FileCache_BufferSelectWithPos(M4OSA_FileCache_Context* apContext)
/**************************************************************/
{
    M4OSA_Int8 i;
    M4OSA_UInt8 selected = 0, j;
    M4OSA_ERR err;
    M4OSA_FilePosition minPos;

    /*********************************************/
    /* 1/ if there is still a new buffer, use it */

    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if(apContext->buffer[i].state == M4OSA_kInitialized)
        {
            selected = i;
            goto end_selection;
        }
    }

    minPos = apContext->buffer[0].filepos;
    selected = 0;
    for(j=1; j<M4OSA_CACHEBUFFER_NB; j++)
    {
        if (apContext->buffer[j].filepos < minPos)
        {
            minPos = apContext->buffer[j].filepos;
            selected = j;
        }
    }

end_selection:
    if (apContext->buffer[selected].filepos > apContext->fileSize )
    {
        /* in case it selects a modified buffer outside real file size,
        in that case, flush all buffers before this one
        unless there will be a seek outside filesize*/
        M4OSA_TRACE3_2("BufferSelectWithPos selected buffer is ouside file b.filepos=%d > fileSize=%d",
                                            apContext->buffer[selected].filepos,
                                             apContext->fileSize );
        M4OSA_FileCache_BuffersFlushUntil(apContext, selected);
    }
    else if ((apContext->buffer[selected].state & M4OSA_kModified) == M4OSA_kModified )
    {
        /* in case it selects a modified buffer inside filesize, simply flush it*/
        err = M4OSA_FileCache_BufferFlush(apContext, selected);
        if (M4NO_ERROR!= err)
        {
            return M4OSA_CACHEBUFFER_NONE;
        }
    }
    M4OSA_TRACE3_1("---------- BufferSelectWithPos returns  i = %d", selected);
     return selected;
}


/* chooses a buffer by overwriting an existing one and returns i */
/**************************************************************/
M4OSA_Int8 M4OSA_FileCache_BufferSelectWithSpace(M4OSA_FileCache_Context* apContext)
/**************************************************************/
{
    M4OSA_Int8 i;
    M4OSA_UInt8 selected = 0;
    M4OSA_UInt32 j, toSort;
    M4OSA_FilePosition bufStat[M4OSA_CACHEBUFFER_NB];
    M4OSA_ERR err;

    /*********************************************/
    /* 1/ if there is still a new buffer, use it */

    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if(apContext->buffer[i].state == M4OSA_kInitialized)
        {
            selected = i;
            goto end_selection;
        }
    }

    i=0;
    /* sort all buffers with order of nbAccessed */
    for(j=0; j<M4OSA_CACHEBUFFER_NB; j++)
    {
        bufStat[i] = apContext->buffer[j].nbAccessed + apContext->buffer[j].timeAccessed*2; /* try hybrid */
        i++;
    }

    toSort = i;
    if (toSort == 0 )
    {
        selected = 0;
        goto end_selection;
    }
    else if (toSort ==1 )
    {
        goto skip_sort;
    }
    else
    {
        M4OSA_FileCache_QS_quickSort(bufStat, toSort);
    }

skip_sort:
    /* take the smallest nbAccessed */
    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if ((M4OSA_Int64) apContext->buffer[i].nbAccessed  + apContext->buffer[i].timeAccessed*2 == bufStat[0]) /* hybrid */
        {
            selected = i;
            goto end_selection;
        }
    }

end_selection:
    if (apContext->buffer[selected].filepos > apContext->fileSize )
    {
        /* in case it selects a modified buffer outside real file size,
        in that case, flush all buffers before this one
        unless there will be a seek outside filesize*/
        M4OSA_FileCache_BuffersFlushUntil(apContext, selected);
    }
    else if ((apContext->buffer[selected].state & M4OSA_kModified) == M4OSA_kModified )
    {
        /* in case it selects a modified buffer inside filesize, simply flush it*/
        err = M4OSA_FileCache_BufferFlush(apContext, selected);
        if (M4NO_ERROR!= err)
        {
            return M4OSA_CACHEBUFFER_NONE;
        }
    }
    M4OSA_TRACE3_1("---------- BufferSelect returns  i = %d", selected);
     return selected;
}


/**************************************************************/
M4OSA_Int8 M4OSA_FileCache_BufferSelectForRead(M4OSA_FileCache_Context* apContext)
/**************************************************************/
{
    M4OSA_Int8 i,j, selected;
    M4OSA_FilePosition min_amount,max_amount;
    M4OSA_Int8 min_i,max_count;
    M4OSA_ERR err;

    /* update nbFillSinceLastAcess field */
    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        apContext->buffer[i].nbFillSinceLastAcess ++;
    }

    /**************************************************/
    /* Plan A/ if there is still a new buffer, use it */

    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if(apContext->buffer[i].state == M4OSA_kInitialized)
        {
            selected = i;
            goto end_selection;
        }
    }

    max_count = M4OSA_CACHEBUFFER_NB;
    max_amount = MAX_FILLS_SINCE_LAST_ACCESS;

    /* Plan B : Scan for dead buffer */
    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        if(apContext->buffer[i].nbFillSinceLastAcess >= (M4OSA_UInt32) max_amount)
        {
            max_amount = apContext->buffer[i].nbFillSinceLastAcess;
            max_count = i;
        }
    }
    if(max_count<M4OSA_CACHEBUFFER_NB)
    {
        M4OSA_TRACE3_2("DEAD BUFFER: %d, %d",max_count,apContext->buffer[max_count].nbFillSinceLastAcess);
        selected = max_count;
        goto end_selection;
    }

    min_i = 0;
    min_amount = M4OSA_CACHEBUFFER_NB;

    /* Select the buffer which is the most "empty" */
    for(i=0; i<M4OSA_CACHEBUFFER_NB; i++)
    {
        j = i % M4OSA_CACHEBUFFER_NB;

        if(apContext->buffer[j].remain < min_amount)
        {
            min_amount = apContext->buffer[j].remain;
            min_i = j;
        }
    }
    selected = min_i;

end_selection:
    if (apContext->buffer[selected].filepos > apContext->fileSize )
    {
        /* in case it selects a modified buffer outside real file size,
        in that case, flush all buffers before this one
        unless there will be a seek outside filesize*/
        M4OSA_FileCache_BuffersFlushUntil(apContext, selected);
    }
    else if ((apContext->buffer[selected].state & M4OSA_kModified) == M4OSA_kModified )
    {
        /* in case it selects a modified buffer inside filesize, simply flush it*/
        err = M4OSA_FileCache_BufferFlush(apContext, selected);
        if (M4NO_ERROR!= err)
        {
            return M4OSA_CACHEBUFFER_NONE;
        }
    }

    return selected;
}


/**************************************************************/
M4OSA_ERR M4OSA_FileCache_CalculateSize(M4OSA_FileCache_Context* apContext)
/**************************************************************/
{
    M4OSA_ERR    err = M4NO_ERROR;
    M4OSA_Int32  ret_val;
    M4OSA_UInt16 errReturned;

    /* go to the end of file*/
    ret_val = apContext->FS->pFctPtr_Seek(apContext->aFileDesc, 0,
                                          M4OSA_kFileSeekEnd,
                                          &errReturned);

    if (ret_val != 0)
    {
        apContext->readFilePos = M4OSA_EOF;
        err = M4OSA_ERR_CREATE(M4_ERR, M4OSA_FILE_READER, errReturned);
        M4OSA_TRACE2_1("M4OSA_FileCache_CalculateSize ERR = 0x%x", err);
    }
    else
    {
        /* Retrieve size of the file */
        apContext->fileSize = apContext->FS->pFctPtr_Tell(apContext->aFileDesc,
                                                                  &errReturned);
        apContext->readFilePos = apContext->fileSize;
    }

    return err;
}

/* _____________________________________________________________  */
/*|                                                             | */
/*| OSAL filesystem functions dependent on Platform FileSystem  | */
/*|_____________________________________________________________| */

/**
 ************************************************************************
 * @brief   Opens a file
 * @note
 * @param   pFileDescriptor :    IN    url of the file
 *          FileModeAccess  :    IN    access mode for opening the file
 *          errno_ffs           :   OUT internal error returned by the filesystem
 * @return  pC              :   internal context
 ************************************************************************
*/
M4OSA_Void* M4OSA_FileSystem_FFS_Open_cache( M4OSA_Void* pFileDescriptor,
                                             M4OSA_UInt32 FileModeAccess,
                                             M4OSA_UInt16* errno_ffs )
{

    M4OSA_FileSystem_FFS_t_cache     *pC = M4OSA_NULL;
    FILE* fp;

    M4OSA_Char  mode[4]            = "";
    M4OSA_Char* pReadString        = (M4OSA_Char*)"r";
    M4OSA_Char* pWriteString    = (M4OSA_Char*)"w";
    M4OSA_Char* pAppendString    = (M4OSA_Char*)"a";
    M4OSA_Char* pBinaryString    = (M4OSA_Char*)"b";
    M4OSA_Char* pPlusString        = (M4OSA_Char*)"+";

    fp = M4OSA_NULL;
    *errno_ffs = 0;

    M4OSA_TRACE3_0("M4OSA_FileSystem_FFS_Open_cache : Open **** \n");

     /************************/
     /*  Verify access mode  */
     /************************/

    /*
    All possible file accesses:

        r : Read only, file must exist

        w : Write only. If the file exists, it is overwritten. If it does not exist, it is created.

        a : write at end of file (append). If the file exists, it is extended. If the file does not exist, it is created.

        r+ : update (i.e. read and write). The file must exist. It is not possible to do a read after a write (or a write after a read)
        unless we reposition the file pointer.

        w+ : creation, to update. If the file exists, it is overwritten. If the files does not exist, it is created.
        a+ : extension and update. If the file does not exist, it is created. If the file exists, the file pointer is put at end of file.

    All possible cases for fileModeAccess parameter:

        Write(2)            w
        WriteRead(3)        r+    // r+b Used by MM
        WriteReadCreate(11)    w+    // w+b Used by MM
        WriteReadAppend(7)    a+
        WriteCreate(10)        w
        WriteAppend(12)        a
        Read                r    // rb Used by MM
        Error
    */


    if ((FileModeAccess & M4OSA_kFileWrite) && (FileModeAccess & M4OSA_kFileRead) && (FileModeAccess & M4OSA_kFileCreate)) /* Used by MM */
    {
        /** "w+" */
        M4OSA_chrNCat(mode, pWriteString, 1);
        M4OSA_chrNCat(mode, pPlusString, 1);
    }
    else if ((FileModeAccess & M4OSA_kFileWrite) && (FileModeAccess & M4OSA_kFileRead) && (FileModeAccess & M4OSA_kFileAppend))
    {
        /** "a+" */
        M4OSA_chrNCat(mode, pAppendString, 1);
        M4OSA_chrNCat(mode, pPlusString, 1);
    }
    else if ((FileModeAccess & M4OSA_kFileWrite) && (FileModeAccess & M4OSA_kFileRead))    /* Used by MM */
    {
        /** "r+" */
        M4OSA_chrNCat(mode, pReadString, 1);
        M4OSA_chrNCat(mode, pPlusString, 1);
    }
    else if ((FileModeAccess & M4OSA_kFileWrite) && (FileModeAccess & M4OSA_kFileCreate))
    {
        /** "w" */
        M4OSA_chrNCat(mode, pWriteString, 1);
    }
    else if ((FileModeAccess & M4OSA_kFileWrite) && (FileModeAccess & M4OSA_kFileAppend))
    {
        /** "a" */
        M4OSA_chrNCat(mode, pAppendString, 1);
    }
    else if (FileModeAccess & M4OSA_kFileRead)
    {
        /** "r" */
        M4OSA_chrNCat(mode, pReadString, 1);
    }
    else if (FileModeAccess & M4OSA_kFileWrite)
    {
        /** "w" */
        M4OSA_chrNCat(mode, pWriteString, 1);
    }
    else
    {
        M4OSA_TRACE1_1("M4OSA_FileSystem_FFS_Open_cache : invalid FileModeAccess = %x", FileModeAccess);
        *errno_ffs = (M4OSA_UInt16)M4ERR_FILE_BAD_MODE_ACCESS;
    }

    /* add the b */
    M4OSA_chrNCat(mode, pBinaryString, 1);

    fp = fopen((const char *) pFileDescriptor, (const char *)mode); /* Open in rb or in r+b*/
    if( fp != NULL )
    {
        pC = (M4OSA_FileSystem_FFS_t_cache *) M4OSA_malloc(sizeof * pC, M4OSA_FILE_READER, (M4OSA_Char*)"M4OSA_FileSystem_FFS_Open_cache");

        if (pC == M4OSA_NULL) return M4OSA_NULL; /*error occured => return NULL pointer*/

        pC->FileDesc = fp;
    }
    else
    {
        switch(errno)
        {
            case ENOENT:
                 M4OSA_DEBUG(M4ERR_FILE_NOT_FOUND, "M4OSA_fileReadOpen: No such file or directory");
                 *errno_ffs=(M4OSA_UInt16)M4ERR_FILE_NOT_FOUND;
                 break;

            case EACCES:
                M4OSA_DEBUG(M4ERR_FILE_LOCKED, "M4OSA_fileReadOpen: Permission denied");
                *errno_ffs=(M4OSA_UInt16)M4ERR_FILE_LOCKED;
                break;

            case EINVAL:
                M4OSA_DEBUG(M4ERR_FILE_BAD_MODE_ACCESS, "M4OSA_fileReadOpen: Invalid Argument");
                *errno_ffs=(M4OSA_UInt16)M4ERR_FILE_BAD_MODE_ACCESS;
                break;

             case EMFILE:
             case ENOSPC:
             case ENOMEM:
                M4OSA_DEBUG(M4ERR_ALLOC, "M4OSA_fileReadOpen: Too many open files");
                *errno_ffs=(M4OSA_UInt16)M4ERR_ALLOC;
                break;

             default:
                M4OSA_DEBUG(M4ERR_NOT_IMPLEMENTED, "M4OSA_fileReadOpen");
                *errno_ffs=(M4OSA_UInt16)M4ERR_NOT_IMPLEMENTED;

        } /* end switch */
    } /* end else */

    return (M4OSA_Void*)pC;
}

/**
 ************************************************************************
 * @brief   Reads data from file
 * @note
 * @param   pContext        :   IN  internal context
 *          data            :    IN  buffer for reading data
 *          size            :    IN    amount of bytes to read
 *          errno_ffs           :   OUT internal error returned by the filesystem
 * @return  ret             :   effective amount of bytes read / -1 if an error occurs
 ************************************************************************
*/
M4OSA_FilePosition M4OSA_FileSystem_FFS_Read_cache( M4OSA_Void* pContext,
                                                    M4OSA_UInt8* data,
                                                    M4OSA_FilePosition size,
                                                    M4OSA_UInt16* errno_ffs )
{
    M4OSA_FileSystem_FFS_t_cache *pC = (M4OSA_FileSystem_FFS_t_cache *)pContext;
    M4OSA_Int32    res;

    M4OSA_TRACE2_1("M4OSA_FileSystem_FFS_Read  size = %ld", size);

    res = -1;

    res = fread(data,sizeof(M4OSA_Char), size, pC->FileDesc);
    if( -1 < res )
    {
        *errno_ffs = M4NO_ERROR;
    }
    else
    {
        *errno_ffs = errno;
    }

    return (M4OSA_FilePosition)res;
}



/**
 ************************************************************************
 * @brief   Writes data to file
 * @note
 * @param   pContext        :   IN  internal context
 *          data            :    IN  buffer with data to write
 *          size            :    IN    amount of bytes to read
 *          errno_ffs           :   OUT internal error returned by the filesystem
 * @return  ret             :   effective amount of bytes read / an error code if an error occurs
 ************************************************************************
*/
M4OSA_FilePosition M4OSA_FileSystem_FFS_Write_cache( M4OSA_Void* pContext,
                                                     M4OSA_UInt8* data,
                                                     M4OSA_FilePosition size,
                                                     M4OSA_UInt16* errno_ffs )
{
    M4OSA_FileSystem_FFS_t_cache *pC = (M4OSA_FileSystem_FFS_t_cache *)pContext;
    M4OSA_Int32    res;

    M4OSA_TRACE2_1("M4OSA_FileSystem_FFS_Write  size = %ld", size);

    res = 0;

    res = fwrite(data,sizeof(M4OSA_Char), size, pC->FileDesc);
    if( -1 < res )
    {
        *errno_ffs = M4NO_ERROR;
    }
    else
    {
        *errno_ffs = errno;
        M4OSA_TRACE1_1("M4OSA_FileSystem_FFS_Write  error", *errno_ffs);
    }

    fflush(pC->FileDesc);

    return (M4OSA_FilePosition)res;
}

/**
 ************************************************************************
 * @brief   Seeks at given position in a file
 * @note
 * @param   pContext        :   IN  internal context
 *          pos             :    IN  amount of bytes for the move
 *          mode            :    IN    kind of seek to perform
 *          errno_ffs           :   OUT internal error returned by the filesystem
 * @return  ret             :   0 on success / any other value if an error occurs
 ************************************************************************
*/
M4OSA_Int32 M4OSA_FileSystem_FFS_Seek_cache( M4OSA_Void* pContext,
                                             M4OSA_FilePosition pos,
                                             M4OSA_FileSeekAccessMode mode,
                                             M4OSA_UInt16* errno_ffs )
{
    M4OSA_FileSystem_FFS_t_cache *pC = (M4OSA_FileSystem_FFS_t_cache *)pContext;

    M4OSA_TRACE2_2("M4OSA_FileSystem_FFS_Seek  pos = %ld  mode = %d", pos, mode);

    switch(mode)
    {
        case M4OSA_kFileSeekBeginning :
            *errno_ffs = fseek(pC->FileDesc, pos, SEEK_SET);
            break;

        case M4OSA_kFileSeekCurrent :
            *errno_ffs= fseek(pC->FileDesc, pos, SEEK_CUR);
            break;

        case M4OSA_kFileSeekEnd :
            *errno_ffs = fseek(pC->FileDesc, pos, SEEK_END);
            break;
    }

    return *errno_ffs;

}

/**
 ************************************************************************
 * @brief   Tells the position of the file pointer
 * @note
 * @param   pContext        :   IN  internal context
 *          errno_ffs           :   OUT internal error returned by the filesystem
 * @return  ret             :   position of the file pointer/ -1 if an error occurs
 ************************************************************************
*/
M4OSA_FilePosition M4OSA_FileSystem_FFS_Tell_cache( M4OSA_Void* pContext,
                                                       M4OSA_UInt16* errno_ffs )
{
    M4OSA_FileSystem_FFS_t_cache *pC = (M4OSA_FileSystem_FFS_t_cache *)pContext;
    M4OSA_FilePosition pos;

    pos = ftell(pC->FileDesc);

    *errno_ffs = 0;

    return pos;
}

/**
 ************************************************************************
 * @brief   Closes the file
 * @note
 * @param   pContext        :   IN  internal context
 *          errno_ffs           :   OUT internal error returned by the filesystem
 * @return  ret             :   0 on success / any other value if an error occurs
 ************************************************************************
*/
M4OSA_Int32 M4OSA_FileSystem_FFS_Close_cache( M4OSA_Void* pContext,
                                                       M4OSA_UInt16* errno_ffs )
{
    M4OSA_FileSystem_FFS_t_cache *pC = (M4OSA_FileSystem_FFS_t_cache *)pContext;

    *errno_ffs = fclose(pC->FileDesc);

    return *errno_ffs;
}

/* __________________________________________________________ */
/*|                                                          |*/
/*|                    OSAL fileCache                        |*/
/*|__________________________________________________________|*/

/**************************************************************/
M4OSA_ERR M4OSA_fileOpen_cache(M4OSA_Context* pContext,
                               M4OSA_Void* pFileDescriptor,
                               M4OSA_UInt32 FileModeAccess)
/**************************************************************/
{
    M4OSA_FileSystem_FctPtr_cache *FS;

    /* Allocate memory for the File System interface */
    FS = (M4OSA_FileSystem_FctPtr_cache *)M4OSA_malloc(sizeof * FS,
                M4OSA_FILE_READER,(M4OSA_Char*)"M4OSA_FileSystem_FctPtr_cache");

    if(M4OSA_NULL == FS)
        return M4ERR_ALLOC;

    FS->pFctPtr_Open = M4OSA_FileSystem_FFS_Open_cache;
    FS->pFctPtr_Read = M4OSA_FileSystem_FFS_Read_cache;
    FS->pFctPtr_Write = M4OSA_FileSystem_FFS_Write_cache;
    FS->pFctPtr_Seek = M4OSA_FileSystem_FFS_Seek_cache;
    FS->pFctPtr_Tell = M4OSA_FileSystem_FFS_Tell_cache;
    FS->pFctPtr_Close = M4OSA_FileSystem_FFS_Close_cache;

    return M4OSA_fileOpen_cache_internal(pContext, pFileDescriptor,
                                                            FileModeAccess, FS);
}

/**
******************************************************************************
* @brief       This method opens the provided fileDescriptor and returns its context.
* @param       pContext:       (OUT) File Cache context.
* @param       pFileDescriptor :       (IN) File Descriptor of the input file.
* @param       FileModeAccess :        (IN) File mode access.
* @return      M4NO_ERROR: there is no error
* @return      M4ERR_PARAMETER pContext or fileDescriptor is NULL
* @return      M4ERR_ALLOC     there is no more memory available
* @return      M4ERR_FILE_BAD_MODE_ACCESS      the file mode access is not correct
* @return      M4ERR_FILE_NOT_FOUND The file can not be opened.
******************************************************************************
*/
M4OSA_ERR M4OSA_fileOpen_cache_internal(M4OSA_Context* pContext,
                                        M4OSA_Void* pFileDescriptor,
                                        M4OSA_UInt32 FileModeAccess,
                                        M4OSA_FileSystem_FctPtr_cache *FS)
{
    M4OSA_FileCache_Context* apContext = M4OSA_NULL;

    M4OSA_ERR   err       = M4NO_ERROR;
    M4OSA_Void* aFileDesc = M4OSA_NULL;
    M4OSA_Bool  buffers_allocated = M4OSA_FALSE;
    M4OSA_UInt16 errReturned = 0;
    M4OSA_Int32 len,name_len;
    M4OSA_Char* pCharFileDesc = (M4OSA_Char*)pFileDescriptor;

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_Time time1 = 0;
    M4OSA_Time time2 = 0;
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

    M4OSA_TRACE2_2("M4OSA_fileOpen_cache fd = %s mode = %d", pFileDescriptor,
                                                                FileModeAccess);

    /*      Check input parameters */
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_PARAMETER, pContext);
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_PARAMETER, pFileDescriptor);
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_PARAMETER, FS);

    *pContext = M4OSA_NULL;

    /* Allocate memory for the File reader context. */
    apContext = (M4OSA_FileCache_Context *)M4OSA_malloc(sizeof(M4OSA_FileCache_Context),
                     M4OSA_FILE_READER, (M4OSA_Char*)"M4OSA_FileCache_Context");

    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_ALLOC, apContext);


#ifdef M4OSA_FILE_CACHE_TIME_MEAS
        M4OSA_FileCache_initTimeMeas(apContext);
        M4OSA_clockGetTime(&time1,1000);
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

    /* Set filesystem interface */
    apContext->FS = FS;

    if (M4OSA_kFileWrite == FileModeAccess)
    {
        FileModeAccess |= M4OSA_kFileWrite | M4OSA_kFileCreate;    /* for VA in case of open with only Write flag, we add the Create */
    }

    /* For VA and VES, we need to add access in read, to write the moov for example */
    /* Add the flag Read in all cases, because Osal File Cache uses read at the same time */
    FileModeAccess |= M4OSA_kFileRead;

    aFileDesc = apContext->FS->pFctPtr_Open(pFileDescriptor, FileModeAccess,
                                                                  &errReturned);

    if (aFileDesc != M4OSA_NULL)
    {
        apContext->IsOpened = M4OSA_TRUE;
    }
    else
    {
        /* converts the error to PSW format*/
        err = M4OSA_ERR_CREATE(M4_ERR, M4OSA_FILE_READER, errReturned);
        M4OSA_TRACE1_2("M4OSA_fileOpen_cache error 0x%x for fd = %s", err,
                                                               pFileDescriptor);
        apContext->IsOpened = M4OSA_FALSE;

        /*free the context and associated FS pointers*/
        if (M4OSA_NULL != apContext) /*should never be null*/
        {
            if (M4OSA_NULL != apContext->FS) /*should never be null*/
            {
                M4OSA_free((M4OSA_MemAddr32)apContext->FS);
            }

            M4OSA_free((M4OSA_MemAddr32)apContext);
            apContext = M4OSA_NULL;
        }

        if (M4NO_ERROR != err) goto cleanup;
    }

    /* Allocate buffers */
    err = M4OSA_FileCache_BuffersInit(apContext);
    buffers_allocated = M4OSA_TRUE;

    if (M4NO_ERROR != err) goto cleanup;

    /* Initialize parameters */
    apContext->fileSize = 0;
    apContext->virtualFileSize = 0;
    apContext->absolutePos = 0;
    apContext->absoluteWritePos = 0;


    apContext->readFilePos = 0;

    /* Retrieve the File Descriptor*/
    apContext->aFileDesc = aFileDesc;

    /* Retrieve the File mode Access */
    apContext->FileAttribute.modeAccess = (M4OSA_FileModeAccess)FileModeAccess;

    apContext->chrono = 0;

#ifdef FILECACHE_STATS
    apContext->nbReadCache = 0;
    apContext->nbWriteCache = 0;

    apContext->nbReadFFS = 0;
    apContext->nbWriteFFS = 0;
#endif

    /*Retrieve the File reader context */
    *pContext= (M4OSA_Context)apContext;

    /* Compute file size */
    err = M4OSA_FileCache_CalculateSize(apContext);

    if (M4NO_ERROR != err) goto cleanup;

    apContext->virtualFileSize = apContext->fileSize;

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_clockGetTime(&time2,1000);
    if (time2>time1)
        apContext->gMyPerfFileTab[fileOpentime] += time2-time1;
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

    M4OSA_mutexOpen(&(apContext->m_mutex));

    /* filename extraction, just for traces  */
    M4OSA_memset(apContext->m_filename, 256, 0);
    len=( M4OSA_chrLength(pCharFileDesc) )+1;
    for( --len ; (len >= 0 && pCharFileDesc[len] != '\\' && pCharFileDesc[len] != '/') ; len-- );
    name_len=M4OSA_chrLength( &pCharFileDesc[len+1] );
    err=M4OSA_chrNCopy(apContext->m_filename, &pCharFileDesc[len+1], name_len);

    M4OSA_TRACE2_2("M4OSA_fileOpen_cache of %s has pC = 0x%x", apContext->m_filename, apContext);

    return M4NO_ERROR;

cleanup:

    /* free context */
    if (M4OSA_NULL != apContext)
    {
        if(buffers_allocated == M4OSA_TRUE)
        {
            M4OSA_FileCache_BuffersFree(apContext);
        }

        if (M4OSA_NULL != apContext)
        {
            M4OSA_free((M4OSA_MemAddr32)apContext);
            apContext = M4OSA_NULL;
        }
        *pContext = M4OSA_NULL;
    }

    return err;
}

/**
******************************************************************************
* @brief       This method reads the 'size' bytes in the core file reader (selected by its 'context')
*                      and writes the data to the 'data' pointer. If 'size' byte can not be read in the core file reader,
*                      'size' parameter is updated to match the correct number of read bytes.
* @param       pContext:       (IN) File reader context.
* @param       pData : (OUT) Data pointer of the read data.
* @param       pSize : (INOUT) Size of the data to read (in byte).
* @return      M4NO_ERROR: there is no error
* @return      M4ERR_PARAMETER pSize, fileDescriptor or pData is NULL
* @return      M4ERR_ALLOC     there is no more memory available
* @return      M4ERR_BAD_CONTEXT       provided context is not a valid one.
******************************************************************************
*/
M4OSA_ERR M4OSA_fileReadData_cache(M4OSA_Context pContext,M4OSA_MemAddr8 pData,
                                                           M4OSA_UInt32* pSize)
{
    M4OSA_FileCache_Context* apContext = (M4OSA_FileCache_Context*) pContext;

    M4OSA_ERR err;
    M4OSA_FilePosition aSize;
    M4OSA_FilePosition copiedSize;
    M4OSA_Int8 selected_buffer, current_buffer;
    M4OSA_Int32 castedSize;

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_Time time1 = 0;
    M4OSA_Time time2 = 0;

    M4OSA_clockGetTime(&time1,1000);
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

    M4OSA_TRACE2_3("M4OSA_fileReadData_cache of %s size=%d at pos=%d  ",
                         apContext->m_filename, *pSize, apContext->absolutePos);

    /* Check input parameters */
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_BAD_CONTEXT, apContext);
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_PARAMETER, pData);
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_PARAMETER, pSize);

    if (apContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;
    }

LOCK

/* 20080125 Start : if *pSize is too high, adjust it to the size left in the file. MI-958*/
    castedSize = * pSize;
    if (castedSize < 0)
    {
        copiedSize = 0;
        err = M4WAR_NO_MORE_AU;
#ifdef M4OSA_FILECACHE_MM
        err = M4WAR_NO_DATA_YET; /* no_data_yet for MM */
#endif
        goto cleanup;
    }
/* 20080125 End : if *pSize is too high, adjust it to the size left in the file. MI-958*/

    /* Prevent reading beyond EOF */
    if((*pSize > 0) && (apContext->absolutePos >= apContext->virtualFileSize)) /* virtual FSize*/
    {
        copiedSize = 0;
        err = M4WAR_NO_MORE_AU; /* for VA and VPS */
#ifdef M4OSA_FILECACHE_MM
        err = M4WAR_NO_DATA_YET; /* no_data_yet for MM */
#endif
        goto cleanup;
    }

/* 20080125 Start : if *pSize is too high, adjust it to the size left in the file. MI-958*/
    if (*pSize > (M4OSA_UInt32)(apContext->virtualFileSize - apContext->absolutePos))
    {
        M4OSA_TRACE1_0("M4OSA_fileReadData_cache : Attempted to read beyond file size, adjusted size");
        *pSize = apContext->virtualFileSize - apContext->absolutePos;
    }
/* 20080125 End : if *pSize is too high, adjust it to the size left in the file. MI-958*/

    /* Check if data can be read from a buffer */
    /* If not, fill one according to quantized positions */
    copiedSize = 0;
    err = M4NO_ERROR;

    selected_buffer = M4OSA_FileCache_BufferMatchToRead(apContext,
                                                        apContext->absolutePos);

    if(selected_buffer == M4OSA_CACHEBUFFER_NONE)
    {

#if defined(BUFFER_SELECT_INITIAL)
        selected_buffer = M4OSA_FileCache_BufferSelectForRead(apContext);
#elif defined(BUFFER_SELECT_WITH_TIME)
        selected_buffer = M4OSA_FileCache_BufferSelectWithTime(apContext);
#elif defined(BUFFER_SELECT_WITH_SPACE)
        selected_buffer = M4OSA_FileCache_BufferSelectWithSpace(apContext);
#elif defined(BUFFER_SELECT_WITH_POS)
        selected_buffer = M4OSA_FileCache_BufferSelectWithPos(apContext);
#endif

        if (M4OSA_CACHEBUFFER_NONE == selected_buffer)
        {
            err = M4ERR_BAD_CONTEXT; /* temporary error code */
            goto cleanup;
        }

        err = M4OSA_FileCache_BufferFill(apContext, selected_buffer,
                                                        apContext->absolutePos);
    }
#ifdef FILECACHE_STATS
    else
    {
        /* bufferMatch has success in read  */
        apContext->nbReadCache++;
    }
#endif /* FILECACHE_STATS */

    if(err != M4NO_ERROR)
    {
        if((err == M4WAR_NO_DATA_YET) && (*pSize <= (M4OSA_UInt32)apContext->buffer[selected_buffer].size))
             err = M4NO_ERROR;
        else goto cleanup;
    }

    M4OSA_TRACE3_3("readData  size = %d  buffer = %d  pos = %d",
                               *pSize, selected_buffer, apContext->absolutePos);

    /* Copy buffer into pData */
    while(((M4OSA_UInt32)copiedSize < *pSize) && (err == M4NO_ERROR))
    {
        aSize = M4OSA_FileCache_BufferCopy(apContext, selected_buffer,
                                           apContext->absolutePos+copiedSize,
                                           *pSize-copiedSize, pData+copiedSize);
        copiedSize += aSize;

        if(aSize == 0)
        {
            err = M4WAR_NO_DATA_YET;
        }
        else
        {
            if((M4OSA_UInt32)copiedSize < *pSize)
            {
                current_buffer = selected_buffer;
                selected_buffer = M4OSA_FileCache_BufferMatchToRead(apContext,
                                             apContext->absolutePos+copiedSize);

                if(selected_buffer == M4OSA_CACHEBUFFER_NONE)
                {
#if defined(BUFFER_SELECT_INITIAL)
                    selected_buffer = M4OSA_FileCache_BufferSelectForRead(apContext);
#elif defined(BUFFER_SELECT_WITH_TIME)
                    selected_buffer = M4OSA_FileCache_BufferSelectWithTime(apContext);
#elif defined(BUFFER_SELECT_WITH_SPACE)
                    selected_buffer = M4OSA_FileCache_BufferSelectWithSpace(apContext);
#elif defined(BUFFER_SELECT_WITH_POS)
                    selected_buffer = M4OSA_FileCache_BufferSelectWithPos(apContext);
#endif

                    if (M4OSA_CACHEBUFFER_NONE == selected_buffer)
                    {
                        err = M4ERR_BAD_CONTEXT; /* temporary error code */
                        goto cleanup;
                    }

                    err = M4OSA_FileCache_BufferFill(apContext, selected_buffer,
                                             apContext->absolutePos+copiedSize);

                    if(err != M4NO_ERROR)
                    {
                        if((err == M4WAR_NO_DATA_YET) && ((*pSize-copiedSize) <= (M4OSA_UInt32)apContext->buffer[selected_buffer].size))
                             err = M4NO_ERROR;
                        else goto cleanup;
                    }
                }
#ifdef FILECACHE_STATS
                else
                {
                    /* bufferMatch has success in read  */
                    apContext->nbReadCache++;
                }
#endif /* FILECACHE_STATS */

            }
        }
    }

cleanup :

    /* Update the new position of the pointer */
    apContext->absolutePos = apContext->absolutePos + copiedSize;

#ifdef M4OSA_FILECACHE_MM
    apContext->absoluteWritePos = apContext->absolutePos;
#endif /* M4OSA_FILECACHE_MM */

    if(err != M4NO_ERROR)
    {
        M4OSA_TRACE1_3("M4OSA_fileReadData_cache size = %d  copied = %d  err = 0x%x",
                                                       *pSize, copiedSize, err);
    }

    /* Effective copied size must be returned */
    *pSize = copiedSize;

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_clockGetTime(&time2,1000);
    if (time2>time1)
        apContext->gMyPerfFileTab[fileReadDatatime] += time2-time1;
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

UNLOCK

    /* Read is done */
    return err;
}


/**
 ************************************************************************
 * @brief      This function writes the 'size' bytes stored at 'data' memory
 *             in the file selected by its context.
 * @note       The caller is responsible for allocating/de-allocating the
 *             memory for 'data' parameter.
 * @note       Moreover the data pointer must be allocated to store at least
 *             'size' bytes.
 * @param      pContext: (IN/OUT) Context of the core file reader
 * @param      pData: (IN) Data pointer of the write data
 * @param      size: (IN) Size of the data to write (in bytes)
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4ERR_BAD_CONTEXT: provided context is not a valid one
 * @return     M4ERR_ALLOC: there is no more memory available
 ************************************************************************/

M4OSA_ERR M4OSA_fileWriteData_cache(M4OSA_Context pContext,M4OSA_MemAddr8 pData,
                                                              M4OSA_UInt32 size)
{
    M4OSA_FileCache_Context* apContext = (M4OSA_FileCache_Context*) pContext;

    M4OSA_ERR err;
    M4OSA_FilePosition aSize;
    M4OSA_FilePosition copiedSize;
    M4OSA_Int8 selected_buffer, current_buffer;

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_Time time1 = 0;
    M4OSA_Time time2 = 0;

    M4OSA_clockGetTime(&time1,1000);
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

    M4OSA_TRACE2_3("M4OSA_fileWriteData_cache of %s size=%d at pos=%d   ",
                      apContext->m_filename, size, apContext->absoluteWritePos);

    /* Check input parameters */
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_BAD_CONTEXT, apContext);
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_PARAMETER, pData);


    if (apContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;
    }

    /*protection*/
    if (apContext->absoluteWritePos > apContext->virtualFileSize)
    {
        M4OSA_TRACE1_0("M4OSA_fileWriteData_cache ERROR : attempting to write beyond EOF");
        return M4WAR_NO_DATA_YET;
    }

LOCK

    /* Check if data has been read into a buffer */
    /* If not, we should read that buffer first and then fill it */
    copiedSize = 0;
    err = M4NO_ERROR;

    selected_buffer = M4OSA_FileCache_BufferMatchToWrite(apContext,
                                                   apContext->absoluteWritePos);

    if(selected_buffer == M4OSA_CACHEBUFFER_NONE)
    {
#if defined(BUFFER_SELECT_INITIAL)
        selected_buffer = M4OSA_FileCache_BufferSelectForWrite(apContext);
#elif defined(BUFFER_SELECT_WITH_TIME)
        selected_buffer = M4OSA_FileCache_BufferSelectWithTime(apContext);
#elif defined(BUFFER_SELECT_WITH_SPACE)
        selected_buffer = M4OSA_FileCache_BufferSelectWithSpace(apContext);
#elif defined(BUFFER_SELECT_WITH_POS)
        selected_buffer = M4OSA_FileCache_BufferSelectWithPos(apContext);
#endif

        if (M4OSA_CACHEBUFFER_NONE == selected_buffer)
        {
            M4OSA_TRACE1_1("M4OSA_fileWriteData_cache ERR1 err=0x%x", err);
            err = M4ERR_BAD_CONTEXT; /* temporary error code */
            goto cleanup;
        }

        if (apContext->absoluteWritePos - M4OSA_CACHEBUFFER_SIZE < apContext->fileSize) /* absolutePos not readfilepo strictly < */
        {
            err = M4OSA_FileCache_BufferFill(apContext, selected_buffer,
                                                   apContext->absoluteWritePos);
        }
        else
        {
            err = M4OSA_FileCache_BufferReinitialize(apContext, selected_buffer,
                                                   apContext->absoluteWritePos);
        }

    }
#ifdef FILECACHE_STATS
    else
    {
        /* bufferMatch has success in write */
        apContext->nbWriteCache++;
    }
#endif /* FILECACHE_STATS */

    if(err != M4NO_ERROR)
    {
        if(err == M4WAR_NO_DATA_YET) /* means the buffer is small, it is at EOF, bufferFill didn't fully fill it*/
             err = M4NO_ERROR;
        else goto cleanup;
    }

    M4OSA_TRACE3_3("writeData  size = %d  buffer = %d  pos = %d", size,
                                  selected_buffer, apContext->absoluteWritePos);

    /* Copy buffer into pData */
    while(((M4OSA_UInt32)copiedSize < size) && (err == M4NO_ERROR))
    {
        aSize = M4OSA_FileCache_BufferModifyContent(apContext, selected_buffer,
                                                    apContext->absoluteWritePos+copiedSize,
                                                    size-copiedSize, pData+copiedSize);
        copiedSize += aSize;

        /* update virtualFileSize in case we write at the end  */
        if (apContext->absoluteWritePos+copiedSize>apContext->virtualFileSize)
        {
            apContext->virtualFileSize = apContext->absoluteWritePos+copiedSize;
            M4OSA_TRACE3_1("virtualFileSize incremented to %d", apContext->virtualFileSize);
        }

        if((M4OSA_UInt32)copiedSize < size)
        {
            current_buffer = selected_buffer;
            selected_buffer = M4OSA_FileCache_BufferMatchToWrite(apContext,
                                        apContext->absoluteWritePos+copiedSize);

            if(selected_buffer == M4OSA_CACHEBUFFER_NONE)
            {
#if defined(BUFFER_SELECT_INITIAL)
                selected_buffer = M4OSA_FileCache_BufferSelectForWrite(apContext);
#elif defined(BUFFER_SELECT_WITH_TIME)
                selected_buffer = M4OSA_FileCache_BufferSelectWithTime(apContext);
#elif defined(BUFFER_SELECT_WITH_SPACE)
                selected_buffer = M4OSA_FileCache_BufferSelectWithSpace(apContext);
#elif defined(BUFFER_SELECT_WITH_POS)
                selected_buffer = M4OSA_FileCache_BufferSelectWithPos(apContext);
#endif

                if (M4OSA_CACHEBUFFER_NONE == selected_buffer)
                {
                    M4OSA_TRACE1_1("M4OSA_fileWriteData_cache ERR2 err=0x%x", err);
                    err = M4ERR_BAD_CONTEXT; /* temporary error code */
                    goto cleanup;
                }

                if (apContext->absoluteWritePos+copiedSize < apContext->fileSize) /* absolutePos not readfilepo strictly < */
                {
                    err = M4OSA_FileCache_BufferFill(apContext, selected_buffer,
                                        apContext->absoluteWritePos+copiedSize);
                }
                else
                {
                    err = M4OSA_FileCache_BufferReinitialize(apContext,
                                                             selected_buffer,
                                                             apContext->absoluteWritePos+copiedSize);
                }


                if(err != M4NO_ERROR)
                {
                    if((err == M4WAR_NO_DATA_YET))
                         err = M4NO_ERROR;
                    else goto cleanup;
                }
            }
#ifdef FILECACHE_STATS
    else  /* (selected_buffer == M4OSA_CACHEBUFFER_NONE) */
    {
        /* bufferMatch has success in write */
        apContext->nbWriteCache++;
    }
#endif /* FILECACHE_STATS */

        }

    }

cleanup :

    /* Update the new position of the pointer */
    apContext->absoluteWritePos = apContext->absoluteWritePos + copiedSize;
#ifdef M4OSA_FILECACHE_MM
    apContext->absolutePos = apContext->absoluteWritePos;
#endif /* M4OSA_FILECACHE_MM */

    if(err != M4NO_ERROR)
    {
        M4OSA_TRACE3_3("M4OSA_fileWriteData_cache size = %d  copied = %d  err = 0x%x",
                                                         size, copiedSize, err);
    }

    /* Effective copied size must be returned */
    size = copiedSize;

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_clockGetTime(&time2,1000);
    if (time2>time1)
        apContext->gMyPerfFileTab[fileWriteDatatime] += time2-time1;
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

UNLOCK

    /* Read is done */
    return err;
}


/**
******************************************************************************
* @brief       This method seeks at the provided position in the core file reader (selected by its 'context').
*              The position is related to the seekMode parameter it can be either :
*              From the beginning (position MUST be positive) : end position = position
*              From the end (position MUST be negative) : end position = file size + position
*              From the current position (signed offset) : end position = current position + position.
* @param       pContext:       (IN) File reader context.
* @param       SeekMode :      (IN) Seek access mode.
* @param       pPosition :     (IN) Position in the file.
* @return      M4NO_ERROR: there is no error
* @return      M4ERR_PARAMETER Seekmode or fileDescriptor is NULL
* @return      M4ERR_ALLOC     there is no more memory available
* @return      M4ERR_BAD_CONTEXT       provided context is not a valid one.
* @return      M4ERR_FILE_INVALID_POSITION the position cannot be reached.
******************************************************************************
*/
M4OSA_ERR M4OSA_fileReadSeek_cache( M4OSA_Context pContext,
                                    M4OSA_FileSeekAccessMode SeekMode,
                                    M4OSA_FilePosition* pPosition)
{
    M4OSA_FileCache_Context* apContext = (M4OSA_FileCache_Context*) pContext;
    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_FilePosition finalPos;

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_Time time1 = 0;
    M4OSA_Time time2 = 0;

    M4OSA_clockGetTime(&time1,1000);
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

    M4OSA_TRACE3_2("M4OSA_fileReadSeek_cache mode = %d pos = %d", SeekMode, *pPosition);

    /* Check input parameters */
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_BAD_CONTEXT, apContext);
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_PARAMETER, pPosition);
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_PARAMETER, SeekMode);

    if (apContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;       /*< The context can not be correct */
    }

LOCK

    /* Go to the desired position */
    switch(SeekMode)
    {
        case M4OSA_kFileSeekBeginning :
            finalPos = *pPosition;
            break;

        case M4OSA_kFileSeekEnd :
            finalPos = apContext->virtualFileSize + *pPosition;
            break;

        case M4OSA_kFileSeekCurrent :
            finalPos = apContext->absolutePos + *pPosition;
            break;

        default :
            UNLOCK
            return M4ERR_PARAMETER; /**< Bad SeekAcess mode */
            break;
    }

    M4OSA_TRACE2_1("M4OSA_fileReadSeek_cache to absolutePos = %d ", finalPos);

/* 20080125 Start : Protect against seek outside file. MI-958*/
    if (finalPos <= apContext->virtualFileSize && finalPos>=0)
    {
        apContext->absolutePos = finalPos;
        *pPosition = finalPos;
    }
    else
    {
        M4OSA_TRACE1_2("M4OSA_fileReadSeek_cache: attempted to seek at %d whilst filesize=%d",
                                          finalPos, apContext->virtualFileSize);
        *pPosition = apContext->absolutePos;    /* keep the previous position */
        //err = M4ERR_FILE_INVALID_POSITION;
        err = M4NO_ERROR;  /* for VA */
    }
/* 20080125 End : Protect against seek outside file. MI-958*/

#ifdef M4OSA_FILECACHE_MM
        apContext->absoluteWritePos = apContext->absolutePos;
#endif /* M4OSA_FILECACHE_MM */

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_clockGetTime(&time2,1000);
    if (time2>time1)
        apContext->gMyPerfFileTab[fileSeektime] += time2-time1;
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

UNLOCK

    /* Return without error */
    return err;
}


/**
******************************************************************************
* @brief       This method seeks at the provided position in the core file reader (selected by its 'context').
*              The position is related to the seekMode parameter it can be either :
*              From the beginning (position MUST be positive) : end position = position
*              From the end (position MUST be negative) : end position = file size + position
*              From the current position (signed offset) : end position = current position + position.
* @param       pContext:       (IN) File reader context.
* @param       SeekMode :      (IN) Seek access mode.
* @param       pPosition :     (IN) Position in the file.
* @return      M4NO_ERROR: there is no error
* @return      M4ERR_PARAMETER Seekmode or fileDescriptor is NULL
* @return      M4ERR_ALLOC     there is no more memory available
* @return      M4ERR_BAD_CONTEXT       provided context is not a valid one.
* @return      M4ERR_FILE_INVALID_POSITION the position cannot be reached.
******************************************************************************
*/
M4OSA_ERR M4OSA_fileWriteSeek_cache( M4OSA_Context pContext,
                                     M4OSA_FileSeekAccessMode SeekMode,
                                     M4OSA_FilePosition* pPosition)
{
    M4OSA_FileCache_Context* apContext = (M4OSA_FileCache_Context*) pContext;
    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_FilePosition finalPos;

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_Time time1 = 0;
    M4OSA_Time time2 = 0;

    M4OSA_clockGetTime(&time1,1000);
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

    M4OSA_TRACE3_2("M4OSA_fileWriteSeek_cache mode = %d pos = %d", SeekMode, *pPosition);

    /* Check input parameters */
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_BAD_CONTEXT, apContext);
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_PARAMETER, pPosition);
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_PARAMETER, SeekMode);

    if (apContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;       /*< The context can not be correct */
    }

LOCK

    /* Go to the desired position */
    switch(SeekMode)
    {
        case M4OSA_kFileSeekBeginning :
            finalPos = *pPosition;
            break;

        case M4OSA_kFileSeekEnd :
            finalPos = apContext->virtualFileSize + *pPosition;
            break;

        case M4OSA_kFileSeekCurrent :
            finalPos = apContext->absoluteWritePos + *pPosition;
            break;

        default :
            UNLOCK
            return M4ERR_PARAMETER; /**< Bad SeekAcess mode */
            break;
    }

    M4OSA_TRACE2_1("M4OSA_fileWriteSeek_cache to absoluteWritePos = %d ", finalPos);

/* 20080125 Start : Protect against seek outside file. MI-958*/
    if (finalPos <= apContext->virtualFileSize && finalPos>=0)
    {
        apContext->absoluteWritePos = finalPos;
        *pPosition = finalPos;
    }
    else
    {
        M4OSA_TRACE1_2("M4OSA_fileWriteSeek_cache: attempted to seek at %d whilst filesize=%d     ",
                                          finalPos, apContext->virtualFileSize);
        *pPosition = apContext->absoluteWritePos;    /* keep the previous position */
        //err = M4ERR_FILE_INVALID_POSITION;
        err = M4NO_ERROR;  /* for VA */
    }
/* 20080125 End : Protect against seek outside file. MI-958*/

#ifdef M4OSA_FILECACHE_MM
    apContext->absolutePos = apContext->absoluteWritePos;
#endif /* M4OSA_FILECACHE_MM */

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_clockGetTime(&time2,1000);
    if (time2>time1)
        apContext->gMyPerfFileTab[fileSeektime] += time2-time1;
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

UNLOCK

    /* Return without error */
    return err;
}

M4OSA_ERR M4OSA_fileFlush_cache( M4OSA_Context pContext)
{
    /* Do nothing, M4OSA_fileCache module manages its caches by itself */

    return M4NO_ERROR;
}
/**
******************************************************************************
* @brief       This method asks the core file reader to close the file (associated to the context).
* @param       pContext:       (IN) File reader context.
* @return      M4NO_ERROR: there is no error
* @return      M4ERR_BAD_CONTEXT       provided context is not a valid one.
******************************************************************************
*/
M4OSA_ERR M4OSA_fileClose_cache(M4OSA_Context pContext)
{
    M4OSA_FileCache_Context* apContext = (M4OSA_FileCache_Context*) pContext;

    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_Int32 aRet_Val = 0;
    M4OSA_UInt16 errReturned = 0;


#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_Time time1 = 0;
    M4OSA_Time time2 = 0;

    M4OSA_clockGetTime(&time1,1000);
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

    M4OSA_TRACE2_1("M4OSA_fileClose_cache pC = 0x%x", pContext);

    /* Check input parameters */
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_BAD_CONTEXT, apContext);

    if (apContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;       /**< The context can not be correct */
    }

LOCK

#ifdef BUFFER_DISPLAY
    M4OSA_FileCache_BufferDisplay(apContext);
#endif

    M4OSA_FileCache_BuffersFlushUntil(apContext, M4OSA_CACHEBUFFER_ALL);

    /* buffer */
    M4OSA_FileCache_BuffersFree(apContext);

    /* Close the file */
    aRet_Val = apContext->FS->pFctPtr_Close(apContext->aFileDesc, &errReturned);

    if (aRet_Val != 0)
    {
        /* converts the error to PSW format*/
        err = M4OSA_ERR_CREATE(M4_ERR, M4OSA_FILE_READER, errReturned);
        M4OSA_TRACE1_1("M4OSA_fileClose_cache ERR1 = 0x%x", err);
    }
    apContext->IsOpened = M4OSA_FALSE;

    /* Free the context */
    M4OSA_free((M4OSA_MemAddr32)apContext->FS);
    M4OSA_free((M4OSA_MemAddr32)apContext->aFileDesc);

#ifdef FILECACHE_STATS
{
    M4OSA_Int32 successRateRead, successRateWrite;

    successRateRead= (apContext->nbReadFFS + apContext->nbReadCache ==0)? (-1) : (apContext->nbReadCache)*100 / (apContext->nbReadCache + apContext->nbReadFFS);

    successRateWrite = (apContext->nbWriteFFS + apContext->nbWriteCache == 0)? (-1) : (apContext->nbWriteCache)*100 / (apContext->nbWriteCache + apContext->nbWriteFFS);

#if defined(BUFFER_SELECT_INITIAL)
    M4OSA_TRACE1_0("BUFFER_SELECT_INITIAL");
#elif defined(BUFFER_SELECT_WITH_TIME)
    M4OSA_TRACE1_0("BUFFER_SELECT_WITH_TIME");
#elif defined(BUFFER_SELECT_WITH_SPACE)
    M4OSA_TRACE1_0("BUFFER_SELECT_WITH_SPACE");
#elif defined(BUFFER_SELECT_WITH_POS)
    M4OSA_TRACE1_0("BUFFER_SELECT_WITH_POS");
#endif

    M4OSA_TRACE1_1("Osal File Cache Stats for %s", apContext->m_filename);
    M4OSA_TRACE1_2("FILECACHE_STATS: nbReadCache=%d / nbReadFFS=%d",
                                  apContext->nbReadCache, apContext->nbReadFFS);
    M4OSA_TRACE1_2("FILECACHE_STATS: nbWriteCache=%d / nbWriteFFS=%d",
                                apContext->nbWriteCache, apContext->nbWriteFFS);
    M4OSA_TRACE1_2("FILECACHE_STATS: Success in reading : %d percent - Success in writing %d percent",
                                             successRateRead, successRateWrite);
    M4OSA_TRACE1_0("---------------------------------------------------------");
}
#endif /* FILECACHE_STATS */

    UNLOCK

    if (apContext->m_mutex != M4OSA_NULL)
    {
        M4OSA_mutexClose(apContext->m_mutex);
        apContext->m_mutex = M4OSA_NULL;
    }

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
        M4OSA_clockGetTime(&time2,1000);
        if (time2>time1)
            apContext->gMyPerfFileTab[fileClosetime] += time2-time1;

        M4OSA_FileCache_displayTimeMeas(apContext);
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

    M4OSA_memset((M4OSA_MemAddr8)apContext, sizeof(M4OSA_FileCache_Context), 0);

    M4OSA_free((M4OSA_MemAddr32)apContext);

    M4OSA_TRACE2_1("M4OSA_fileClose_cache leaving with err = 0x%x", err);

    /* Return without error */
    return err;
}

/**
******************************************************************************
* @brief       This method asks the core file reader to set the value associated with the optionID.
*                      The caller is responsible for allocating/de-allocating the memory of the value field.
* @note        The options handled by the component depend on the implementation of the component.
* @param       pContext:       (IN) Execution context.
* @param       OptionId :      (IN) Id of the option to set.
* @param       OptionValue :   (IN) Value of the option.
* @return      M4NO_ERROR: there is no error
* @return      M4ERR_BAD_CONTEXT       pContext is NULL
* @return      M4ERR_BAD_OPTION_ID the option id is not valid.
* @return      M4ERR_NOT_IMPLEMENTED The option is not implemented yet.
******************************************************************************
*/
M4OSA_ERR M4OSA_fileSetOption_cache(M4OSA_Context pContext,
                                    M4OSA_OptionID OptionID,
                                    M4OSA_DataOption OptionValue)
{
    M4OSA_FileCache_Context* apContext = (M4OSA_FileCache_Context*) pContext;

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_Time time1 = 0;
    M4OSA_Time time2 = 0;

    M4OSA_clockGetTime(&time1,1000);
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

    /* Check input parameters */
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_BAD_CONTEXT, apContext);

    if (apContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;       /**< The context can not be correct */
    }

    /* Set the desired option if it is avalaible */
    switch(OptionID)
    {
        case M4OSA_kFileReadGetFileSize :       /**< Get size of the file, limited to 32 bit size */
        case M4OSA_kFileReadGetFileAttribute :  /**< Get the file attribute*/
        case M4OSA_kFileReadGetURL :            /**< Get the directory + name of the file */
        case M4OSA_kFileReadIsEOF :             /**< See if we are at the end of the file */
        case M4OSA_kFileReadGetFilePosition :   /**< Get file position */
            return M4ERR_READ_ONLY;
            break;

        case M4OSA_kFileWriteDescMode:
            return M4NO_ERROR;                    /* for MM */

        default :                               /**< Bad option ID */
            return M4ERR_BAD_OPTION_ID;
            break;
    }

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_clockGetTime(&time2,1000);
    if (time2>time1)
        apContext->gMyPerfFileTab[fileSetOptiontime] += time2-time1;
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

    /* Return without error */
    return M4NO_ERROR;
}

/**
******************************************************************************
* @brief       This method asks the core file reader to return the value associated with the optionID.
*                      The caller is responsible for allocating/de-allocating the memory of the value field.
* @note        The options handled by the component depend on the implementation of the component.
* @param       pContext:       (IN) Execution context.
* @param       OptionId :      (IN) Id of the option to set.
* @param       pOptionValue :  (OUT) Value of the option.
* @return      M4NO_ERROR: there is no error
* @return      M4ERR_BAD_CONTEXT       pContext is NULL
* @return      M4ERR_BAD_OPTION_ID the option id is not valid.
* @return      M4ERR_NOT_IMPLEMENTED The option is not implemented yet.
******************************************************************************
*/
M4OSA_ERR M4OSA_fileGetOption_cache(M4OSA_Context pContext,
                                    M4OSA_OptionID OptionID,
                                    M4OSA_DataOption* pOptionValue)
{
    M4OSA_FileCache_Context* apContext = (M4OSA_FileCache_Context*) pContext;
    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_Bool isEof;

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_Time time1 = 0;
    M4OSA_Time time2 = 0;

    M4OSA_clockGetTime(&time1,1000);
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */


    /*  Check input parameters */
    M4ERR_CHECK_NULL_RETURN_VALUE(M4ERR_BAD_CONTEXT, apContext);

    if (apContext->IsOpened != M4OSA_TRUE)
    {
        return M4ERR_BAD_CONTEXT;       /**< The context can not be correct */
    }

LOCK

    /* Get the desired option if it is avalaible */
    switch(OptionID)
    {
        /* Get File Size */
        case M4OSA_kFileReadGetFileSize:/**< Get size of the file, limited to 32 bit size */
            M4OSA_TRACE2_1("M4OSA_fileGetOption_cache ReadGetFileSize return filesize = %d ",
                                                    apContext->virtualFileSize);
            (*(M4OSA_UInt32 *)pOptionValue) = apContext->virtualFileSize; /* return virtual */
            break;


        case M4OSA_kFileWriteGetFileSize:/**< Get size of the file, limited to 32 bit size */
            M4OSA_TRACE2_1("M4OSA_fileGetOption_cache WriteGetFileSize return filesize = %d ",
                                                    apContext->virtualFileSize);
            (*(M4OSA_UInt32 *)pOptionValue) = apContext->virtualFileSize; /* return virtual */
            break;

        /* Check End of file Occurs */
        case M4OSA_kFileReadIsEOF :     /**< See if we are at the end of the file */
            isEof = (apContext->absolutePos >= apContext->virtualFileSize) ? M4OSA_TRUE : M4OSA_FALSE; /* virtual */
            (*(M4OSA_Bool *)pOptionValue) = isEof;
            M4OSA_TRACE2_1("M4OSA_fileGetOption_cache ReadIsEOF return isEof=%d ",
                                                                         isEof);
            break;

        /* Get File Position */
        case M4OSA_kFileReadGetFilePosition :   /**< Get file position */
            M4OSA_TRACE2_1("M4OSA_fileGetOption_cache ReadGetFilePosition return rpos=%d ",
                                                        apContext->absolutePos);
            *(M4OSA_FilePosition *)pOptionValue = apContext->absolutePos;
            break;

        /* Get File Position */
        case M4OSA_kFileWriteGetFilePosition :    /**< Get file position */
            M4OSA_TRACE2_1("M4OSA_fileGetOption_cache WriteGetFilePosition return wpos=%d ",
                                                   apContext->absoluteWritePos);
            *(M4OSA_FilePosition *)pOptionValue = apContext->absoluteWritePos;
            break;

        /* Get Attribute */
        case M4OSA_kFileReadGetFileAttribute :  /**< Get the file attribute = access mode */
            M4OSA_TRACE2_1("M4OSA_fileGetOption_cache ReadGetFileAttribute return mode=%d ",
                                           apContext->FileAttribute.modeAccess);
            (*(M4OSA_FileAttribute *)pOptionValue).modeAccess = apContext->FileAttribute.modeAccess;
            break;
   /** Get the reader context for read & write file. (M4OSA_Context*)*/
        case M4OSA_kFileWriteGetReaderContext:
            M4OSA_TRACE2_1("M4OSA_fileGetOption_cache WriteGetReaderContext return c=0x%x ",
                                                                     apContext);
            (*(M4OSA_Context *)pOptionValue) = apContext;
            break;
        default:
            /**< Bad option ID */
            UNLOCK
            return  M4ERR_BAD_OPTION_ID;
            break;
    }

#ifdef M4OSA_FILE_CACHE_TIME_MEAS
    M4OSA_clockGetTime(&time2,1000);
    if (time2>time1)
        apContext->gMyPerfFileTab[fileGetOptiontime] += time2-time1;
#endif /* M4OSA_FILE_CACHE_TIME_MEAS */

    UNLOCK


    /*Return without error */
    return err;
}

/* For VA */
M4OSA_ERR M4OSA_fileExtrafTruncate_cache(M4OSA_Context context, M4OSA_UInt32 length)
{
    M4OSA_ERR err = M4NO_ERROR;
    M4OSA_UInt16 result = M4OSA_FALSE;
    M4OSA_FileCache_Context* apContext = context;


    FILE* filedesc1 = ((M4OSA_FileSystem_FFS_t_cache*) ( apContext->aFileDesc))->FileDesc;

    result = ftruncate(filedesc1->_file, length);

    if(result != 0)
    {
        err = errno;
        M4OSA_TRACE1_1("SetEndOfFile returns err: 0x%x\n", err);
        return M4OSA_ERR_CREATE(M4_ERR, M4OSA_FILE_EXTRA, err);
    }
    return M4NO_ERROR;
}
#ifdef M4OSA_FILE_CACHE_TIME_MEAS

/**************************************************************/
void M4OSA_FileCache_initTimeMeas(M4OSA_Context pContext)
/**************************************************************/
{
    M4OSA_FileCache_Context* apContext = (M4OSA_FileCache_Context*) pContext;
    M4OSA_Time time1 = 0;

    memset(apContext->gMyPerfFileTab, 0, sizeof(apContext->gMyPerfFileTab)); //Reset perf measurement array

    M4OSA_clockGetTime(&time1,1000);
    apContext->gMyPerfFileTab[enum_size] = time1;         //to compute total application time

}

/**************************************************************/
void M4OSA_FileCache_displayTimeMeas(M4OSA_Context pContext)
/**************************************************************/
{
    M4OSA_FileCache_Context* apContext = (M4OSA_FileCache_Context*) pContext;

    M4OSA_Time globalfileperfmeas = 0;
    M4OSA_Time time2 = 0;
    M4OSA_UInt32 i=0;

    M4OSA_clockGetTime(&time2,1000);

    /* Time spent in application */
    time2 = time2-apContext->gMyPerfFileTab[enum_size];

    /* Time spent in File System procedures */
    for (i=0; i<enum_size; i++)
        globalfileperfmeas += apContext->gMyPerfFileTab[i];

    M4OSA_TRACE1_1("Osal File Cache Time measurement for %s ",
                                                         apContext->m_filename);
    M4OSA_TRACE1_2("Application time =%d, fileCache total time =%d",
                                               (M4OSA_Int32)time2,
                                               (M4OSA_Int32)globalfileperfmeas);
    M4OSA_TRACE1_4("Opentime:%d, ReadDatatime:%d, WriteDatatime: %d, Seektime:%d",
                  (M4OSA_Int32)apContext->gMyPerfFileTab[fileOpentime] ,
                  (M4OSA_Int32)apContext->gMyPerfFileTab[fileReadDatatime] ,
                  (M4OSA_Int32)apContext->gMyPerfFileTab[fileWriteDatatime] ,
                  (M4OSA_Int32)apContext->gMyPerfFileTab[fileSeektime] );
    M4OSA_TRACE1_4("GetOptiontime:%d, SetOptiontime:%d, ExternalFlush: %d, Closetime: %d",
                  (M4OSA_Int32)apContext->gMyPerfFileTab[fileGetOptiontime] ,
                  (M4OSA_Int32)apContext->gMyPerfFileTab[fileSetOptiontime],
                  (M4OSA_Int32)apContext->gMyPerfFileTab[fileExternalFlushtime],
                  (M4OSA_Int32)apContext->gMyPerfFileTab[fileClosetime]);
    M4OSA_TRACE1_0("--------------------------------------------------------------------");
}

#endif /* M4OSA_FILE_INTERFACE_MM_TIME_MEAS */
