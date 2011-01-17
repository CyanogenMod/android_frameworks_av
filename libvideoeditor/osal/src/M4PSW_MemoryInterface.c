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
 * @file   M4PSW_MemoryInterface.c
 * @brief  Memory Interface
 * @note   Implementation of the osal memory functions
 *************************************************************************
*/

#include <stdlib.h>
#include <memory.h>

#include <time.h>
#include "M4OSA_Memory.h"
#ifndef M4VPS_ADVANCED_MEMORY_MANAGER
/**
 ************************************************************************
 * @fn         M4OSA_MemAddr32 M4OSA_malloc(M4OSA_UInt32 size,
 *                                          M4OSA_CoreID coreID,
 *                                          M4OSA_Char* string)
 * @brief      this function allocates a memory block (at least 32 bits aligned)
 * @note
 * @param      size (IN): size of allocated block in bytes
 * @param      coreID (IN): identification of the caller component
 * @param      string (IN): description of the allocated block (null terminated)
 * @return     address of the allocated block, M4OSA_NULL if no memory available
 ************************************************************************
*/

M4OSA_MemAddr32 M4OSA_malloc(M4OSA_UInt32 size,
                             M4OSA_CoreID coreID,
                             M4OSA_Char* string)
{
    M4OSA_MemAddr32 Address = M4OSA_NULL;

    /**
     * If size is 0, malloc on WIN OS allocates a zero-length item in
     * the heap and returns a valid pointer to that item.
     * On other platforms, malloc could returns an invalid pointer
     * So, DON'T allocate memory of 0 byte */
    if (size == 0)
    {
        return Address;
    }

    if (size%4 != 0)
    {
        size = size + 4 - (size%4);
    }

    Address = (M4OSA_MemAddr32) malloc(size);

    return Address;
}


/**
 ************************************************************************
 * @fn         M4OSA_Void M4OSA_free(M4OSA_MemAddr32 address)
 * @brief      this function free the provided memory block
 * @note       As in stlib.h, this function does nothing if address is NULL.
 * @param      address (IN): address of the block to free
 * @return     none
 ************************************************************************
*/

M4OSA_Void M4OSA_free (M4OSA_MemAddr32 address)
{
    free(address);
}


/**
 ************************************************************************
 * @fn         M4OSA_Void M4OSA_memset(M4OSA_MemAddr8 block,
 *                                     M4OSA_UInt32 size,
 *                                     M4OSA_UInt8 value)
 * @brief      this function initializes the provided memory block with value
 * @note
 * @param      block (IN): address of block to fill
 * @param      size  (IN): size of the provided block
 * @param      value (IN): value used for initialization
 * @return     none
 ************************************************************************
*/

M4OSA_Void M4OSA_memset(M4OSA_MemAddr8 block,
                        M4OSA_UInt32 size,
                        M4OSA_UInt8 value)
{
    memset((void*)block, value, size);
}


/**
 ************************************************************************
 * @fn         M4OSA_Void M4OSA_memcpy(M4OSA_MemAddr8 outputBlock,
 *                                     M4OSA_MemAddr8 inputBlock,
 *                                     M4OSA_UInt32 size)
 * @brief      this function copies 'size' bytes from inputBlock to outputBlock
 * @note
 * @param      outputBlock (IN): address of block to fill
 * @param      inputBlock  (IN): address of the input block
 * @param      size (IN): size of the block to copy (in bytes)
 * @return     none
 ************************************************************************
*/

M4OSA_Void M4OSA_memcpy(M4OSA_MemAddr8 outputBlock,
                        M4OSA_MemAddr8 inputBlock,
                        M4OSA_UInt32 size)
{
    memcpy((void*)outputBlock, (void*)inputBlock,  size);
}

/**
 ************************************************************************
 * @fn         M4OSA_MemAddr8 M4OSA_memmove(M4OSA_MemAddr8 outputBlock, M4OSA_MemAddr8 inputBlock, M4OSA_UInt32 size)
 * @brief      this function moves 'size' bytes from inputBlock to outputBlock
 *               unlike M4OSA_memcpy, the two buffers can have an overlap.
 * @note       increment memcpy byte number (François VALETTE)
 * @param      outputBlock (IN): address of block to fill
 * @param      inputBlock  (IN): address of the input block
 * @param      size (IN): size of the block to copy (in bytes)
 * @return     address of the output block, i.e. the first parameter
 ************************************************************************
*/
M4OSA_MemAddr8 M4OSA_memmove(M4OSA_MemAddr8 outputBlock,
                         M4OSA_MemAddr8 inputBlock,
                         M4OSA_UInt32 size)
{
   return memmove((void*)outputBlock, (void*)inputBlock,  size);
}

/**
 ************************************************************************
 * @fn         M4OSA_Int32 M4OSA_memcmp(M4OSA_MemAddr8 address1, M4OSA_MemAddr8 address2, M4OSA_UInt32 size)
 * @brief      this function compares the first 'size' bytes of address1 and
               'address2' and return a value indicating their relationship.
 * @note
 * @param      address1 (IN): memory address 1
 * @param      address2 (IN): memory address 2
 * @param      size (IN): size of the block to compare (in bytes)
 * @return     +1, if first bytes of adress1 are smaller than those of address2
 * @return      0, if both blocks are identical
 * @return    -1, if first bytes of address1 are bigger than those of address2
 ************************************************************************
*/

M4OSA_Int32 M4OSA_memcmp(M4OSA_MemAddr8 address1,
                         M4OSA_MemAddr8 address2,
                         M4OSA_UInt32 size)
{
    M4OSA_Int32 i32_result = memcmp(address1, address2, size);
    if (i32_result > 0) {
        return 1;
    }
    else if (i32_result < 0) {
        return((M4OSA_Int32)-1);
    }
    return 0;
}




#endif

