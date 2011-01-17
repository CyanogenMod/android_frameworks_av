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
 * @file         M4OSA_String.c
 ************************************************************************
*/

#include "M4OSA_Debug.h"
#include "M4OSA_Memory.h"
#include "M4OSA_Types.h"
#include "M4OSA_Error.h"
#include "M4OSA_CharStar.h"
#include "M4OSA_FileCommon.h"
#include "M4OSA_String_priv.h"
#include "M4OSA_String.h"


/**
 ************************************************************************
 * @brief      This function creates an empty M4OSA_String
 * @note
 * @param      pStrOut
 * @return     M4OSA_ERROR
 ************************************************************************
*/
M4OSA_ERR M4OSA_strCreate(M4OSA_String* pStrOut)
{
    M4OSA_strStruct* pStr = M4OSA_NULL;

    M4OSA_TRACE1_1("M4OSA_strCreate\t\tM4OSA_String* 0x%x", pStrOut);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrOut, M4ERR_PARAMETER, "M4OSA_strCreate");
    M4OSA_DEBUG_IF2(M4OSA_NULL != *pStrOut, M4ERR_STR_BAD_STRING,
                                                             "M4OSA_strCreate");

    /* Allocate the output M4OSA_String */
    pStr = (M4OSA_strStruct*)M4OSA_malloc(sizeof(M4OSA_strStruct), M4OSA_STRING,
        (M4OSA_Char*)"M4OSA_strPrivCreate: output string");

    /* Check memory allocation error */
    if(M4OSA_NULL == pStr)
    {
        *pStrOut = M4OSA_NULL ;

        M4OSA_DEBUG(M4ERR_ALLOC, "M4OSA_strPrivCreate");

        return M4ERR_ALLOC;
    }

    pStr->coreID = M4OSA_STRING;
    pStr->pui8_buffer = M4OSA_NULL;
    pStr->ui32_length = 0;
    pStr->ui32_size = 0;

    *pStrOut = pStr;

    return M4NO_ERROR;
}




/**
 ************************************************************************
 * @brief      This function reset the M4OSA_String
 * @note
 * @param      str_in
 * @return     M4OSA_ERROR
 ************************************************************************
 */
M4OSA_ERR M4OSA_strReset(M4OSA_String str_in)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_Char* pBuffer;

    M4OSA_TRACE1_1("M4OSA_strReset\t\tM4OSA_String* 0x%x", str_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strReset");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                              "M4OSA_strReset");

    pBuffer = pStr->pui8_buffer;

    if(M4OSA_NULL != pBuffer)
    {
        M4OSA_free((M4OSA_MemAddr32)pBuffer);

        pStr->pui8_buffer = M4OSA_NULL;
    }

    pStr->ui32_length = 0;
    pStr->ui32_size = 0;


    return M4NO_ERROR;
}




/**
 ************************************************************************
 * @brief      This function free the memory of the input M4OSA_String
 * @note
 * @param      str_in
 * @return     M4OSA_ERROR
 ************************************************************************
 */
M4OSA_ERR M4OSA_strDestroy(M4OSA_String str_in)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;

    M4OSA_TRACE1_1("M4OSA_strDestroy\t\tM4OSA_String 0x%x", str_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strDestroy");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                            "M4OSA_strDestroy");


    /* Free M4OSA_String buffer */
    M4OSA_free((M4OSA_MemAddr32)(pStr->pui8_buffer));

    /* Free M4OSA_String structure memory */
    M4OSA_free((M4OSA_MemAddr32)pStr);


    return M4NO_ERROR;
}




/**
 ************************************************************************
 * @brief     str_in content
 * @note
 * @param      str_in
 * @param      pChar
 * @return     M4OSA_ERROR
 ************************************************************************
 */
M4OSA_ERR M4OSA_strSetCharContent(M4OSA_String str_in, M4OSA_Char *pChar)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;

    M4OSA_TRACE1_2("M4OSA_strSetContent\t\tM4OSA_String 0x%x\tM4OSA_Char*"
        " 0x%x", str_in, pChar);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetContent");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pChar, M4ERR_PARAMETER,
                                                         "M4OSA_strSetContent");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                         "M4OSA_strSetContent");

    return M4OSA_strPrivSet(pStr, pChar, M4OSA_chrLength(pChar));
}




/**
 ************************************************************************
* @brief      This function returns, in pac_content, the "C-String" of str_in
 * @note
 * @param      str_in
 * @param      pac_content
 * @return     M4OSA_ERROR
 ************************************************************************
*/
M4OSA_ERR M4OSA_strGetCharContent(M4OSA_String str_in, M4OSA_Char** ppchar)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;

    M4OSA_TRACE1_2("M4OSA_strGetContent\t\tM4OSA_String 0x%x\tM4OSA_Char**"
        " 0x%x", str_in, ppchar);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strGetContent");
    M4OSA_DEBUG_IF2(M4OSA_NULL == ppchar, M4ERR_PARAMETER,
                                                         "M4OSA_strGetContent");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                         "M4OSA_strGetContent");

    *ppchar = pStr->pui8_buffer;


    return M4NO_ERROR;
}

M4OSA_ERR M4OSA_strSetChar(M4OSA_String str_in, M4OSA_Char c_in)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err;

    M4OSA_TRACE1_2("M4OSA_strSetChar\t\tM4OSA_String 0x%x\tM4OSA_Char %c",
        str_in, c_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetChar");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                            "M4OSA_strSetChar");


    err = M4OSA_strPrivRealloc(pStr, 1);

    if(M4OSA_ERR_IS_ERROR(err))
    {
        return err;
    }

    pStr->pui8_buffer[0] = c_in;
    pStr->pui8_buffer[1] = '\0';
    pStr->ui32_length    = 1;


    return M4NO_ERROR;
}




M4OSA_ERR M4OSA_strGetChar(M4OSA_String str_in, M4OSA_Char* pc_out)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;

    M4OSA_TRACE1_2("M4OSA_strGetChar\t\tM4OSA_String 0x%x\tM4OSA_Char* 0x%x",
        str_in, pc_out);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pc_out, M4ERR_PARAMETER, "M4OSA_strGetChar");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strGetChar");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                            "M4OSA_strGetChar");

    if(pStr->ui32_length == 0)
    {
        return M4ERR_STR_CONV_FAILED;
    }

    *pc_out = pStr->pui8_buffer[0];


    return M4NO_ERROR;
}

M4OSA_ERR M4OSA_strSetInt8(M4OSA_String str_in, M4OSA_Int8 i8_in,
                           M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char aui8_buffer[8];
    M4OSA_ERR err_code;

    M4OSA_TRACE1_2("M4OSA_strSetInt8\t\tM4OSA_String 0x%x\tM4OSA_Int8 %d",
        str_in, i8_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetInt8");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                            "M4OSA_strSetInt8");


    /* Convert input number into "C-String" */
    switch(base)
    {
    case M4OSA_kstrDec:
        {
            err_code = M4OSA_chrSPrintf(aui8_buffer, 8, (M4OSA_Char*)"%d", i8_in);
            break;
        }

    case M4OSA_kstrHexa:
        {
            if(i8_in < 0)
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 8, (M4OSA_Char*)"-%X",
                                                                        -i8_in);
            }
            else
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 8, (M4OSA_Char*)"%X",
                                                                         i8_in);
            }
            break;
        }

    case M4OSA_kstrOct:
        {
            if(i8_in < 0)
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 8, (M4OSA_Char*)"-%o",
                                                                        -i8_in);
            }
            else
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 8, (M4OSA_Char*)"%o",
                                                                         i8_in);
            }
            break;
        }

    default:
        {
            return M4ERR_PARAMETER;
        }
    }


    if(M4OSA_ERR_IS_ERROR(err_code))
    {
        return err_code;
    }

    /* Calculate M4OSA_String content length */
    ui32_length = M4OSA_chrLength(aui8_buffer) ;


    return M4OSA_strPrivSet(pStr, aui8_buffer, ui32_length);
}

M4OSA_ERR M4OSA_strGetInt8(M4OSA_String str_in, M4OSA_Int8* pi8_out,
                           M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strGetInt8\t\tM4OSA_String 0x%x\tM4OSA_Int8* 0x%x\t"
        "M4OSA_strNumBase %d", str_in, pi8_out, base);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strGetInt8");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pi8_out, M4ERR_PARAMETER, "M4OSA_strGetInt8");
    M4OSA_DEBUG_IF2((base != M4OSA_kstrDec) && (base != M4OSA_kstrHexa) &&
        (base != M4OSA_kstrOct), M4ERR_PARAMETER, "M4OSA_strGetInt8");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID,M4ERR_STR_BAD_STRING,
                                                            "M4OSA_strGetInt8");


    err_code = M4OSA_chrGetInt8(pStr->pui8_buffer, pi8_out, M4OSA_NULL, base);

    if(M4NO_ERROR != err_code)
    {
        M4OSA_DEBUG(M4ERR_STR_CONV_FAILED, "M4OSA_strGetInt8");

        return M4ERR_STR_CONV_FAILED;
    }


    return M4NO_ERROR;
}



M4OSA_ERR M4OSA_strSetUInt8(M4OSA_String str_in, M4OSA_UInt8 ui8_in,
                            M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char aui8_buffer[4];
    M4OSA_ERR err_code;
    M4OSA_Char* pFormat;

    M4OSA_TRACE1_2("M4OSA_strSetUInt8\t\tM4OSA_String* 0x%x\tM4OSA_UInt8 %d",
        str_in, ui8_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER,  "M4OSA_strSetUInt8");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                           "M4OSA_strSetUInt8");

    if (base == M4OSA_kchrDec)
    {
        pFormat = (M4OSA_Char*)"%u";
    }
    else if (base == M4OSA_kchrHexa)
    {
        pFormat = (M4OSA_Char*)"%X";
    }
    else if (base == M4OSA_kchrOct)
    {
        pFormat = (M4OSA_Char*)"%o";
    }
    else
    {
        pFormat = M4OSA_NULL;
    }

    /* Convert input number into "C-String" */
    err_code = M4OSA_chrSPrintf(aui8_buffer, 4, pFormat, ui8_in);

    if(M4OSA_ERR_IS_ERROR(err_code))
    {
        return err_code;
    }

    /* Calculate M4OSA_String content length */
    ui32_length = M4OSA_chrLength(aui8_buffer) ;


    return M4OSA_strPrivSet(pStr, aui8_buffer, ui32_length);
}



M4OSA_ERR M4OSA_strGetUInt8(M4OSA_String str_in,
                            M4OSA_UInt8* pui8_out,
                            M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strGetUInt8\t\tM4OSA_String 0x%x\tM4OSA_UInt8* 0x%x\t"
        "M4OSA_strNumBase %d", str_in, pui8_out,  base);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strGetUInt8");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pui8_out, M4ERR_PARAMETER,
                                                           "M4OSA_strGetUInt8");
    M4OSA_DEBUG_IF2((base != M4OSA_kstrDec) && (base != M4OSA_kstrHexa)
        && (base != M4OSA_kstrOct), M4ERR_PARAMETER, "M4OSA_strGetUInt8");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                           "M4OSA_strGetUInt8");


    err_code = M4OSA_chrGetUInt8(pStr->pui8_buffer, pui8_out, M4OSA_NULL, base);

    if(M4NO_ERROR != err_code)
    {
        M4OSA_DEBUG(M4ERR_STR_CONV_FAILED, "M4OSA_strGetUInt8");

        return M4ERR_STR_CONV_FAILED;
    }

    return M4NO_ERROR;
}



M4OSA_ERR M4OSA_strSetInt16(M4OSA_String str_in, M4OSA_Int16 i16_in,
                            M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char aui8_buffer[8];
    M4OSA_ERR err_code;

    M4OSA_TRACE1_2("M4OSA_strSetInt16\t\tM4OSA_String* 0x%x\tM4OSA_Int16 %d",
        str_in, i16_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetInt16");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                           "M4OSA_strSetInt16");

    /* Convert input number into "C-String" */
    switch(base)
    {
    case M4OSA_kstrDec:
        {
            err_code = M4OSA_chrSPrintf(aui8_buffer, 8, (M4OSA_Char*)"%d",
                                                                        i16_in);
            break;
        }

    case M4OSA_kstrHexa:
        {
            if(i16_in < 0)
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 8, (M4OSA_Char*)"-%X",
                                                                       -i16_in);
            }
            else
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 8, (M4OSA_Char*)"%X",
                                                                        i16_in);
            }
            break;
        }

    case M4OSA_kstrOct:
        {
            if(i16_in < 0)
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 8, (M4OSA_Char*)"-%o",
                                                                       -i16_in);
            }
            else
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 8, (M4OSA_Char*)"%o",
                                                                        i16_in);
            }
            break;
        }

    default:
        {
            return M4ERR_PARAMETER;
        }
    }

    if(M4OSA_ERR_IS_ERROR(err_code))
    {
        return err_code;
    }

    /* Calculate M4OSA_String content length */
    ui32_length = M4OSA_chrLength(aui8_buffer) ;

    return M4OSA_strPrivSet(pStr, aui8_buffer, ui32_length);
}



M4OSA_ERR M4OSA_strGetInt16(M4OSA_String str_in, M4OSA_Int16* pi16_out,
                            M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strGetInt16\t\tM4OSA_String 0x%x\tM4OSA_Int16* 0x%x"
        "\tM4OSA_strNumBase %d", str_in, pi16_out, base);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strGetInt16");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pi16_out, M4ERR_PARAMETER,
                                                           "M4OSA_strGetInt16");
    M4OSA_DEBUG_IF2((base != M4OSA_kstrDec) && (base != M4OSA_kstrHexa)
            && (base != M4OSA_kstrOct),M4ERR_PARAMETER, "M4OSA_strGetInt16");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                           "M4OSA_strGetInt16");

    err_code = M4OSA_chrGetInt16(pStr->pui8_buffer, pi16_out, M4OSA_NULL, base);

    if(M4NO_ERROR != err_code)
    {
        M4OSA_DEBUG(M4ERR_STR_CONV_FAILED, "M4OSA_strGetInt16");

        return M4ERR_STR_CONV_FAILED;
    }

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strSetUInt16(M4OSA_String str_in, M4OSA_UInt16 ui16_in,
                             M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char aui8_buffer[8];
    M4OSA_ERR err_code;
    M4OSA_Char* pFormat;

    M4OSA_TRACE1_2("M4OSA_strSetUInt16\t\tM4OSA_String* 0x%x\tM4OSA_UInt16 %d",
        str_in, ui16_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetUInt16");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                         "M4OSA_strSetUInt16");

    if (M4OSA_kchrDec == base)
    {
        pFormat = (M4OSA_Char*)"%u";
    }
    else if (M4OSA_kchrHexa == base)
    {
        pFormat = (M4OSA_Char*)"%X";
    }
    else if (M4OSA_kchrOct == base)
    {
        pFormat = (M4OSA_Char*)"%o";
    }
    else
    {
        pFormat = M4OSA_NULL;
    }

    /* Convert input number into "C-String" */
    err_code = M4OSA_chrSPrintf(aui8_buffer, 8, pFormat, ui16_in);

    if(M4OSA_ERR_IS_ERROR(err_code))
    {
        return err_code;
    }

    /* Calculate M4OSA_String content length */
    ui32_length = M4OSA_chrLength(aui8_buffer) ;


    return M4OSA_strPrivSet(pStr, aui8_buffer, ui32_length);
}


M4OSA_ERR M4OSA_strGetUInt16(M4OSA_String str_in, M4OSA_UInt16* pui16_out,
                             M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strGetUInt16\t\tM4OSA_String 0x%x\tM4OSA_UInt16* "
        "0x%x\tM4OSA_strNumBase %d", str_in, pui16_out, base);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strGetUInt16");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pui16_out, M4ERR_PARAMETER,
                                                          "M4OSA_strGetUInt16");
    M4OSA_DEBUG_IF2((base != M4OSA_kstrDec) && (base != M4OSA_kstrHexa)
            && (base != M4OSA_kstrOct), M4ERR_PARAMETER, "M4OSA_strGetUInt16");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                          "M4OSA_strGetUInt16");

    err_code = M4OSA_chrGetUInt16(pStr->pui8_buffer, pui16_out, M4OSA_NULL,
                                                                          base);

    if(M4NO_ERROR != err_code)
    {
        M4OSA_DEBUG(M4ERR_STR_CONV_FAILED, "M4OSA_strGetUInt16");

        return M4ERR_STR_CONV_FAILED;
    }

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strSetInt32(M4OSA_String str_in, M4OSA_Int32 i32_in,
                            M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char aui8_buffer[16];
    M4OSA_ERR err_code;
    M4OSA_Char* pFormat;

    M4OSA_TRACE1_2("M4OSA_strSetInt32\t\tM4OSA_String* 0x%x\tM4OSA_Int32 %d",
        str_in, i32_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetInt32");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                           "M4OSA_strSetInt32");

    if (M4OSA_kchrDec == base)
    {
        pFormat = (M4OSA_Char*)"%d";
    }
    else if (M4OSA_kchrHexa == base)
    {
        pFormat = (M4OSA_Char*)"%X";
    }
    else if (M4OSA_kchrOct == base)
    {
        pFormat = (M4OSA_Char*)"%o";
    }
    else
    {
        pFormat = M4OSA_NULL;
    }

    /* Convert input number into "C-String" */
    switch(base)
    {
    case M4OSA_kstrDec:
        {
            err_code = M4OSA_chrSPrintf(aui8_buffer, 16, (M4OSA_Char*)"%d",
                                                                        i32_in);
            break;
        }

    case M4OSA_kstrHexa:
        {
            if(i32_in < 0)
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 16, (M4OSA_Char*)"-%X",
                                                                       -i32_in);
            }
            else
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 16, (M4OSA_Char*)"%X",
                                                                        i32_in);
            }
            break;
        }

    case M4OSA_kstrOct:
        {
            if(i32_in < 0)
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 16, (M4OSA_Char*)"-%o",
                                                                       -i32_in);
            }
            else
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 16, (M4OSA_Char*)"%o",
                                                                        i32_in);
            }
            break;
        }

    default:
        {
            return M4ERR_PARAMETER;
        }
    }

    if(M4OSA_ERR_IS_ERROR(err_code))
    {
        return err_code;
    }

    /* Calculate M4OSA_String content length */
    ui32_length = M4OSA_chrLength(aui8_buffer) ;

    return M4OSA_strPrivSet(pStr, aui8_buffer, ui32_length);
}

M4OSA_ERR M4OSA_strGetInt32(M4OSA_String str_in,
                            M4OSA_Int32* pi32_out,
                            M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strGetInt32\t\tM4OSA_String 0x%x\tM4OSA_Int32* 0x%x"
        "\tM4OSA_strNumBase %d", str_in, pi32_out, base);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strGetInt32");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pi32_out, M4ERR_PARAMETER,
                                                           "M4OSA_strGetInt32");
    M4OSA_DEBUG_IF2((base != M4OSA_kstrDec) && (base != M4OSA_kstrHexa)
        && (base != M4OSA_kstrOct), M4ERR_PARAMETER, "M4OSA_strGetInt32");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                           "M4OSA_strGetInt32");

    err_code = M4OSA_chrGetInt32(pStr->pui8_buffer, pi32_out, M4OSA_NULL, base);

    if(M4NO_ERROR != err_code)
    {
        M4OSA_DEBUG(M4ERR_STR_CONV_FAILED, "M4OSA_strGetInt32");

        return M4ERR_STR_CONV_FAILED;
    }

    return M4NO_ERROR;
}

M4OSA_ERR M4OSA_strSetUInt32(M4OSA_String str_in, M4OSA_UInt32 ui32_in,
                             M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char aui8_buffer[12];
    M4OSA_ERR err_code;
    M4OSA_Char* pFormat;

    M4OSA_TRACE1_2("M4OSA_strSetUInt32\t\tM4OSA_String* 0x%x\tM4OSA_UInt32 %d",
        str_in, ui32_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetUInt32");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                          "M4OSA_strSetUInt32");

    if (M4OSA_kchrDec == base)
    {
        pFormat = (M4OSA_Char*)"%u";
    }
    else if (M4OSA_kchrHexa == base)
    {
        pFormat = (M4OSA_Char*)"%X";
    }
    else if (M4OSA_kchrOct == base)
    {
        pFormat = (M4OSA_Char*)"%o";
    }
    else
    {
        pFormat = M4OSA_NULL;
    }

    /* Convert input number into "C-String" */
    err_code = M4OSA_chrSPrintf(aui8_buffer, 12, pFormat, ui32_in);

    if(M4OSA_ERR_IS_ERROR(err_code))
    {
        return err_code;
    }

    /* Calculate M4OSA_String content length */
    ui32_length = M4OSA_chrLength(aui8_buffer) ;


    return M4OSA_strPrivSet(pStr, aui8_buffer, ui32_length);
}


M4OSA_ERR M4OSA_strGetUInt32(M4OSA_String str_in, M4OSA_UInt32* pui32_out,
                             M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strGetUInt32\t\tM4OSA_String 0x%x\tM4OSA_UInt32* "
        "0x%x\tM4OSA_strNumBase %d", str_in, pui32_out, base);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strGetUInt32");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pui32_out, M4ERR_PARAMETER,
                                                          "M4OSA_strGetUInt32");
    M4OSA_DEBUG_IF2((base != M4OSA_kstrDec) && (base != M4OSA_kstrHexa)
        && (base != M4OSA_kstrOct), M4ERR_PARAMETER, "M4OSA_strGetUInt32");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                          "M4OSA_strGetUInt32");

    err_code = M4OSA_chrGetUInt32(pStr->pui8_buffer, pui32_out,
                    M4OSA_NULL, base);

    if(M4NO_ERROR != err_code)
    {
        M4OSA_DEBUG(M4ERR_STR_CONV_FAILED, "M4OSA_strGetUInt32");

        return M4ERR_STR_CONV_FAILED;
    }

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strSetInt64(M4OSA_String str_in, M4OSA_Int64 i64_in,
                            M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char aui8_buffer[24];
    M4OSA_ERR err_code;


    M4OSA_TRACE1_2("M4OSA_strSetInt64\t\tM4OSA_String* 0x%x\tM4OSA_Int64 0x%x",
        str_in, &i64_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetInt64");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                           "M4OSA_strSetInt64");

    /* Convert input number into "C-String" */
    switch(base)
    {
    case M4OSA_kstrDec:
        {
            err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"%lld",
                                                                        i64_in);
            break;
        }

    case M4OSA_kstrHexa:
        {
            if(M4OSA_INT64_IS_POSITIVE(i64_in))
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"%llX",
                                                                        i64_in);
            }
            else
            {
                M4OSA_INT64_NEG(i64_in, i64_in);
                err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"-%llX",
                                                                        i64_in);
            }
            break;
        }

    case M4OSA_kstrOct:
        {
            if(M4OSA_INT64_IS_POSITIVE(i64_in))
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"%llo",
                                                                        i64_in);
            }
            else
            {
                M4OSA_INT64_NEG(i64_in, i64_in);
                err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"-%llo",
                                                                        i64_in);
            }
            break;
        }

    default:
        {
            return M4ERR_PARAMETER;
        }
    }

    if(M4OSA_ERR_IS_ERROR(err_code))
    {
        return err_code;
    }

    /* Calculate M4OSA_String content length */
    ui32_length = M4OSA_chrLength(aui8_buffer) ;

    return M4OSA_strPrivSet(pStr, aui8_buffer, ui32_length);
}

M4OSA_ERR M4OSA_strGetInt64(M4OSA_String str_in, M4OSA_Int64* pi64_out,
                            M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strGetInt64\t\tM4OSA_String 0x%x\tM4OSA_Int64* 0x%x"
        "\tM4OSA_strNumBase %d", str_in, pi64_out, base);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strGetInt64");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pi64_out, M4ERR_PARAMETER,
                                                           "M4OSA_strGetInt64");
    M4OSA_DEBUG_IF2((base != M4OSA_kstrDec) && (base != M4OSA_kstrHexa)
        && (base != M4OSA_kstrOct), M4ERR_PARAMETER, "M4OSA_strGetInt64");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                           "M4OSA_strGetInt64");

    err_code = M4OSA_chrGetInt64(pStr->pui8_buffer, pi64_out, M4OSA_NULL, base);

    if(M4NO_ERROR != err_code)
    {
        M4OSA_DEBUG(M4ERR_STR_CONV_FAILED, "M4OSA_strGetInt64");

        return M4ERR_STR_CONV_FAILED;
    }

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strSetDouble(M4OSA_String str_in, M4OSA_Double d_in)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char aui8_buffer[24];
    M4OSA_ERR err_code;

    M4OSA_TRACE1_2("M4OSA_strSetDouble\t\tM4OSA_String* 0x%x\tM4OSA_Double* "
        "0x%x", str_in, &d_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetDouble");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                          "M4OSA_strSetDouble");

    /* Convert input number into "C-String" */
    err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"%e", d_in);

    if(M4OSA_ERR_IS_ERROR(err_code))
    {
        return err_code;
    }

    /* Calculate M4OSA_String content length */
    ui32_length = M4OSA_chrLength(aui8_buffer) ;

    return M4OSA_strPrivSet(pStr, aui8_buffer, ui32_length);
}


M4OSA_ERR M4OSA_strGetDouble(M4OSA_String str_in, M4OSA_Double* pd_out)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_2("M4OSA_strGetDouble\t\tM4OSA_String 0x%x\tM4OSA_Double* "
        "0x%x", str_in, pd_out);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strGetDouble");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pd_out, M4ERR_PARAMETER, "M4OSA_strGetDouble");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                          "M4OSA_strGetDouble");

    err_code = M4OSA_chrGetDouble(pStr->pui8_buffer, pd_out, M4OSA_NULL);
    if(M4NO_ERROR != err_code)
    {
        M4OSA_DEBUG(M4ERR_STR_CONV_FAILED, "M4OSA_strGetDouble");

        return M4ERR_STR_CONV_FAILED;
    }

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strSetFilePosition(M4OSA_String str_in, M4OSA_FilePosition fpos_in,
                                   M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char aui8_buffer[24];
    M4OSA_ERR err_code;


    M4OSA_TRACE1_2("M4OSA_strSetFilePosition\t\tM4OSA_String* 0x%x\t"
        "M4OSA_FilePosition* 0x%x", str_in, &fpos_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER,
                                                    "M4OSA_strSetFilePosition");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                    "M4OSA_strSetFilePosition");


    /* Convert input number into "C-String" */
    switch(base)
    {
    case M4OSA_kstrDec:
        {
            err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"%aad",
                                                                       fpos_in);
            break;
        }

    case M4OSA_kstrHexa:
        {
            if(M4OSA_FPOS_IS_POSITIVE(fpos_in))
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"%aaX",
                                                                       fpos_in);
            }
            else
            {
                M4OSA_FPOS_NEG(fpos_in, fpos_in);
                err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"-%aaX",
                                                                       fpos_in);
            }
            break;
        }

    case M4OSA_kstrOct:
        {
            if(M4OSA_FPOS_IS_POSITIVE(fpos_in))
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"%aao",
                                                                       fpos_in);
            }
            else
            {
                M4OSA_FPOS_NEG(fpos_in, fpos_in);
                err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"-%aao",
                                                                       fpos_in);
            }
            break;
        }

    default:
        {
            return M4ERR_PARAMETER;
        }
    }

    if(M4OSA_ERR_IS_ERROR(err_code))
    {
        return err_code;
    }

    /* Calculate M4OSA_String content length */
    ui32_length = M4OSA_chrLength(aui8_buffer) ;

    return M4OSA_strPrivSet(pStr, aui8_buffer, ui32_length);
}

M4OSA_ERR M4OSA_strGetFilePosition(M4OSA_String str_in, M4OSA_FilePosition* pfpos_out,
                                   M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strGetFilePosition\t\tM4OSA_String 0x%x\t"
        "M4OSA_FilePosition* 0x%x\t\tM4OSA_strNumBase %d",
        str_in, pfpos_out, base);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER,
                                                    "M4OSA_strGetFilePosition");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pfpos_out, M4ERR_PARAMETER,
                                                    "M4OSA_strGetFilePosition");
    M4OSA_DEBUG_IF2((base != M4OSA_kstrDec) && (base != M4OSA_kstrHexa)
        && (base != M4OSA_kstrOct), M4ERR_PARAMETER, "M4OSA_strGetFilePosition");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                    "M4OSA_strGetFilePosition");

    err_code = M4OSA_chrGetFilePosition(pStr->pui8_buffer, pfpos_out,
                        M4OSA_NULL, base);

    if(M4NO_ERROR != err_code)
    {
        M4OSA_DEBUG(M4ERR_STR_CONV_FAILED, "M4OSA_strGetFilePosition");

        return M4ERR_STR_CONV_FAILED;
    }

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strSetTime(M4OSA_String str_in, M4OSA_Time t_in,
                           M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char aui8_buffer[24];
    M4OSA_ERR err_code;

    M4OSA_TRACE1_2("M4OSA_strSetDouble\t\tM4OSA_String* 0x%x\tM4OSA_Time* 0x%x",
        str_in, &t_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetTime");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                            "M4OSA_strSetTime");

    /* Convert input number into "C-String" */
    switch(base)
    {
    case M4OSA_kstrDec:
        {
            err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"%tmd",
                                                                          t_in);
            break;
        }

    case M4OSA_kstrHexa:
        {
            if(M4OSA_TIME_IS_POSITIVE(t_in))
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"%tmX",
                                                                          t_in);
            }
            else
            {
                M4OSA_TIME_NEG(t_in, t_in);
                err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"-%tmX",
                                                                          t_in);
            }
            break;
        }

    case M4OSA_kstrOct:
        {
            if(M4OSA_TIME_IS_POSITIVE(t_in))
            {
                err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"%tmo",
                                                                          t_in);
            }
            else
            {
                M4OSA_TIME_NEG(t_in, t_in);
                err_code = M4OSA_chrSPrintf(aui8_buffer, 24, (M4OSA_Char*)"-%tmo",
                                                                          t_in);
            }
            break;
        }

    default:
        {
            return M4ERR_PARAMETER;
        }
    }

    if(M4OSA_ERR_IS_ERROR(err_code))
    {
        return err_code;
    }

    /* Calculate M4OSA_String content length */
    ui32_length = M4OSA_chrLength(aui8_buffer) ;

    return M4OSA_strPrivSet(pStr, aui8_buffer, ui32_length);
}


M4OSA_ERR M4OSA_strGetTime(M4OSA_String str_in, M4OSA_Time* pt_out,
                           M4OSA_strNumBase base)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strGetTime\t\tM4OSA_String 0x%x\tM4OSA_Time* 0x%x"
        "\tM4OSA_strNumBase %d", str_in, pt_out, base);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strGetTime");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pt_out, M4ERR_PARAMETER, "M4OSA_strGetTime");
    M4OSA_DEBUG_IF2((base != M4OSA_kstrDec) && (base != M4OSA_kstrHexa)
        && (base != M4OSA_kstrOct), M4ERR_PARAMETER, "M4OSA_strGetTime");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID,M4ERR_STR_BAD_STRING,
                                                            "M4OSA_strGetTime");

    err_code = M4OSA_chrGetTime(pStr->pui8_buffer, pt_out, M4OSA_NULL, base);

    if(M4NO_ERROR != err_code)
    {
        M4OSA_DEBUG(M4ERR_STR_CONV_FAILED, "M4OSA_strGetTime");

        return M4ERR_STR_CONV_FAILED;
    }

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strGetLength(M4OSA_String str_in, M4OSA_UInt32* pui32_len)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;

    M4OSA_TRACE1_2("M4OSA_strGetLength\t\tM4OSA_String 0x%x\tM4OSA_UInt32* "
        "0x%x", str_in, pui32_len);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strGetLength");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pui32_len, M4ERR_PARAMETER, "M4OSA_strGetLength");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING, "M4OSA_strGetLength");

    /* Get the M4OSA_StringStuct length field */
    *pui32_len = pStr->ui32_length ;

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strCompare(M4OSA_String str_in1, M4OSA_String str_in2,
                           M4OSA_Int32* pi32_result)
{
    M4OSA_strStruct* pStr1 = (M4OSA_strStruct*)str_in1;
    M4OSA_strStruct* pStr2 = (M4OSA_strStruct*)str_in2;
    M4OSA_UInt32 length, length1, length2;
    M4OSA_Int32 result;
    M4OSA_UInt32 i;
    M4OSA_Char* buffer1;
    M4OSA_Char* buffer2;
    M4OSA_Char* pTmp1;
    M4OSA_Char* pTmp2;

    M4OSA_TRACE1_3("M4OSA_strCompare\t\tM4OSA_String 0x%x\tM4OSA_String 0x%x\t"
        "M4OSA_UInt32* 0x%x", str_in1, str_in2, pi32_result);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr1, M4ERR_PARAMETER, "M4OSA_strCompare");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr2, M4ERR_PARAMETER, "M4OSA_strCompare");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pi32_result, M4ERR_PARAMETER, "M4OSA_strCompare");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr1->coreID, M4ERR_STR_BAD_STRING, "M4OSA_strCompare");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr2->coreID, M4ERR_STR_BAD_STRING, "M4OSA_strCompare");

    buffer1 = pStr1->pui8_buffer;
    buffer2 = pStr2->pui8_buffer;

    length1 = pStr1->ui32_length;
    length2 = pStr2->ui32_length;

    length = (length1 < length2) ? length1 : length2;

    pTmp1 = (M4OSA_Char*)M4OSA_malloc(2 * length * sizeof(M4OSA_Char),
                M4OSA_STRING, (M4OSA_Char*)"M4OSA_strCompare");

    M4OSA_CHECK_MALLOC(pTmp1, "M4OSA_strCompare");

    pTmp2 = pTmp1 + length;

    M4OSA_memcpy(pTmp1, buffer1, length);

    M4OSA_memcpy(pTmp2, buffer2, length);

    for(i=0; i<length; i++)
    {
        pTmp1[i] = M4OSA_chrToLower(buffer1[i]);
        pTmp2[i] = M4OSA_chrToLower(buffer2[i]);
    }

    M4OSA_chrNCompare(pTmp1, pTmp2, length, &result);

    M4OSA_free((M4OSA_MemAddr32)pTmp1);

    if(result != 0)
    {
        *pi32_result = result;
    }
    else
    {
        if (length1 == length2)
        {
            *pi32_result = 0;
        }
        else if (length1  > length2)
        {
            *pi32_result = 1;
        }
        else
        {
            *pi32_result = -1;
        }
    }

    return M4NO_ERROR;
}

M4OSA_ERR M4OSA_strCompareSubStr(M4OSA_String str_in1,
                                 M4OSA_UInt32 ui32_offset1,
                                 M4OSA_String str_in2,
                                 M4OSA_UInt32 ui32_offset2,
                                 M4OSA_UInt32* pui32_num,
                                 M4OSA_Int32* pi32_result)
{
    M4OSA_strStruct* pStr1 = (M4OSA_strStruct*)str_in1;
    M4OSA_strStruct* pStr2 = (M4OSA_strStruct*)str_in2;
    M4OSA_Char* pBuffer1;
    M4OSA_Char* pBuffer2;
    M4OSA_Char* pTmp1;
    M4OSA_Char* pTmp2;
    M4OSA_UInt32 length1, length2, i;
    M4OSA_ERR err_code, return_code = M4NO_ERROR;

    M4OSA_TRACE1_5("M4OSA_strCompareSubStr\t\tM4OSA_String 0x%x\tM4OSA_UInt32 "
        "%d\tM4OSA_String 0x%x\tM4OSA_UInt32 %d\tM4OSA_UInt32* 0x%x"
        "\tM4OSA_Int32* 0x%x", str_in1, ui32_offset1, str_in2,
        ui32_offset2, *pui32_num);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr1, M4ERR_PARAMETER,
                                                      "M4OSA_strCompareSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr2, M4ERR_PARAMETER,
                                                      "M4OSA_strCompareSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pi32_result, M4ERR_PARAMETER,
                                                      "M4OSA_strCompareSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pui32_num, M4ERR_PARAMETER,
                                                      "M4OSA_strCompareSubStr");
    M4OSA_DEBUG_IF2(*pui32_num == 0, M4ERR_PARAMETER, "M4OSA_strCompareSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr1->coreID, M4ERR_STR_BAD_STRING,
                                                      "M4OSA_strCompareSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr2->coreID, M4ERR_STR_BAD_STRING,
                                                      "M4OSA_strCompareSubStr");
    M4OSA_DEBUG_IF2(ui32_offset1 >= pStr1->ui32_length, M4ERR_STR_OVERFLOW,
                                                      "M4OSA_strCompareSubStr");
    M4OSA_DEBUG_IF2(ui32_offset2 >= pStr2->ui32_length, M4ERR_STR_OVERFLOW,
                                                      "M4OSA_strCompareSubStr");

    length1 = pStr1->ui32_length - ui32_offset1;
    length2 = pStr2->ui32_length - ui32_offset2;

    pBuffer1 = pStr1->pui8_buffer + ui32_offset1;
    pBuffer2 = pStr2->pui8_buffer + ui32_offset2;

    if(length1 < *pui32_num)
    {
        *pui32_num = length1;

        return_code = M4WAR_STR_OVERFLOW;
    }

    if(length2 < *pui32_num)
    {
        *pui32_num = length2;

        return_code = M4WAR_STR_OVERFLOW;
    }

    pTmp1 = (M4OSA_Char*)M4OSA_malloc(2 * (*pui32_num) * sizeof(M4OSA_Char),
            M4OSA_STRING, (M4OSA_Char*)"M4OSA_strCompareSubStr");

    M4OSA_CHECK_MALLOC(pTmp1, "M4OSA_strCompareSubStr");

    pTmp2 = pTmp1 + (*pui32_num);

    M4OSA_memcpy(pTmp1, pBuffer1, *pui32_num);

    M4OSA_memcpy(pTmp2, pBuffer2, *pui32_num);

    for(i=0; i<(*pui32_num); i++)
    {
        pTmp1[i] = M4OSA_chrToLower(pBuffer1[i]);
        pTmp2[i] = M4OSA_chrToLower(pBuffer2[i]);
    }

    err_code = M4OSA_chrNCompare(pTmp1, pTmp2, *pui32_num, pi32_result);

    M4OSA_DEBUG_IF2((M4OSA_ERR)M4ERR_PARAMETER == err_code, M4ERR_PARAMETER,
                                   "M4OSA_strCompareSubStr: M4OSA_chrNCompare");

    M4OSA_free((M4OSA_MemAddr32)pTmp1);

    return return_code;
}


M4OSA_ERR M4OSA_strCaseCompare(M4OSA_String str_in1, M4OSA_String str_in2,
                               M4OSA_Int32* pi32_result)
{
    M4OSA_strStruct* pStr1 = (M4OSA_strStruct*)str_in1;
    M4OSA_strStruct* pStr2 = (M4OSA_strStruct*)str_in2;
    M4OSA_UInt32 length, length1, length2;
    M4OSA_Char *pBuffer1, *pBuffer2;
    M4OSA_Int32 result;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strCaseCompare\t\tM4OSA_String 0x%x\tM4OSA_String "
        "0x%x\tM4OSA_UInt32* 0x%x", str_in1, str_in2, pi32_result);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr1, M4ERR_PARAMETER, "M4OSA_strCaseCompare");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr2, M4ERR_PARAMETER, "M4OSA_strCaseCompare");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pi32_result,M4ERR_PARAMETER,
                                                        "M4OSA_strCaseCompare");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr1->coreID, M4ERR_STR_BAD_STRING,
                                                        "M4OSA_strCaseCompare");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr2->coreID, M4ERR_STR_BAD_STRING,
                                                        "M4OSA_strCaseCompare");

    length1 = pStr1->ui32_length;
    length2 = pStr2->ui32_length;

    /** NB:  Never use this expression "i = (value1 == value2) ? x: y;"
     * because that doens't compile on other platforms (ADS for example)
     * Use: if(value1 == value2)
     *        { i= x; ..etc
     */
    if (M4OSA_NULL == pStr1->pui8_buffer)
    {
        pBuffer1 = (M4OSA_Char*)"";
    }
    else
    {
        pBuffer1 = pStr1->pui8_buffer;
    }

    if (M4OSA_NULL == pStr2->pui8_buffer)
    {
        pBuffer2 = (M4OSA_Char*)"";
    }
    else
    {
        pBuffer2 = pStr2->pui8_buffer;
    }

    if ((length1 < length2))
    {
        length = length1;
    }
    else
    {
        length =  length2;
    }

    err_code = M4OSA_chrNCompare(pBuffer1, pBuffer2, length, &result);

    M4OSA_DEBUG_IF2((M4OSA_ERR)M4ERR_PARAMETER == err_code, M4ERR_PARAMETER,
                                     "M4OSA_strCaseCompare: M4OSA_chrNCompare");

    if (result != 0)
    {
        *pi32_result = result;
    }
    else if (length1 > length2)
    {
        *pi32_result = 1 ;
    }
    else
    {
        *pi32_result = -1;
    }

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @author     Nicolas Santini (PDSL-P)
 * @author     Hilaire Verschuere (PDSL-P)
 * @brief
 * @note
 * @param      str_in
 * @param
 * @return     M4OSA_ERROR
 * @date       - 2002-12-32: creation
 ************************************************************************
 */
M4OSA_ERR M4OSA_strCaseCompareSubStr(M4OSA_String str_in1,
                                     M4OSA_UInt32 ui32_offset1,
                                     M4OSA_String str_in2,
                                     M4OSA_UInt32 ui32_offset2,
                                     M4OSA_UInt32* pui32_num,
                                     M4OSA_Int32* pi32_result)
{
    M4OSA_strStruct* pStr1 = (M4OSA_strStruct*)str_in1;
    M4OSA_strStruct* pStr2 = (M4OSA_strStruct*)str_in2;
    M4OSA_Char* pBuffer1;
    M4OSA_Char* pBuffer2;
    M4OSA_UInt32 length1, length2;
    M4OSA_ERR err_code = M4NO_ERROR;

    M4OSA_TRACE1_5("M4OSA_strCaseCompareSubStr\t\tM4OSA_String 0x%x\t"
        "M4OSA_UInt32 %d\tM4OSA_String 0x%x\tM4OSA_UInt32 %d\t"
        "M4OSA_UInt32* 0x%x\tM4OSA_Int32* 0x%x", str_in1,
        ui32_offset1, str_in2, ui32_offset2, *pui32_num);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr1, M4ERR_PARAMETER,
                                                  "M4OSA_strCaseCompareSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr2, M4ERR_PARAMETER,
                                                  "M4OSA_strCaseCompareSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pi32_result, M4ERR_PARAMETER,
                                                  "M4OSA_strCaseCompareSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pui32_num, M4ERR_PARAMETER,
                                                  "M4OSA_strCaseCompareSubStr");
    M4OSA_DEBUG_IF2(*pui32_num == 0, M4ERR_PARAMETER,
                                                  "M4OSA_strCaseCompareSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr1->coreID, M4ERR_STR_BAD_STRING,
                                                  "M4OSA_strCaseCompareSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr2->coreID, M4ERR_STR_BAD_STRING,
                                                  "M4OSA_strCaseCompareSubStr");
    M4OSA_DEBUG_IF2(ui32_offset1 >= pStr1->ui32_length, M4ERR_STR_OVERFLOW,
                                                  "M4OSA_strCaseCompareSubStr");
    M4OSA_DEBUG_IF2(ui32_offset2 >= pStr2->ui32_length, M4ERR_STR_OVERFLOW,
                                                  "M4OSA_strCaseCompareSubStr");


    length1 = pStr1->ui32_length - ui32_offset1;
    length2 = pStr2->ui32_length - ui32_offset2;

    pBuffer1 = pStr1->pui8_buffer + ui32_offset1;
    pBuffer2 = pStr2->pui8_buffer + ui32_offset2;

    if(length1 < *pui32_num)
    {
        *pui32_num = length1;

        err_code = M4WAR_STR_OVERFLOW;
    }

    if(length2 < *pui32_num)
    {
        *pui32_num = length2;

        err_code = M4WAR_STR_OVERFLOW;
    }

    M4OSA_chrNCompare(pBuffer1, pBuffer2, *pui32_num, pi32_result);

    return err_code;
}


M4OSA_ERR M4OSA_strSpan(M4OSA_String str_in, M4OSA_Char* charset,
                        M4OSA_UInt32* pui32_pos)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char* pBuffer;
    M4OSA_ERR err_code;


    M4OSA_TRACE1_3("M4OSA_strSpan\t\tM4OSA_String 0x%x\tM4OSA_Char* 0x%x\t"
        "M4OSA_UInt32* 0x%x", str_in, charset, pui32_pos);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSpan");
    M4OSA_DEBUG_IF2(M4OSA_NULL == charset, M4ERR_PARAMETER, "M4OSA_strSpan");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pui32_pos, M4ERR_PARAMETER, "M4OSA_strSpan");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                               "M4OSA_strSpan");

    pStr = (M4OSA_strStruct*)str_in ;

    if(*pui32_pos >= pStr->ui32_length)
    {
        return M4ERR_STR_OVERFLOW;
    }

    pBuffer = pStr->pui8_buffer + *pui32_pos;

    err_code = M4OSA_chrSpan(pBuffer, charset, &ui32_length);

    M4OSA_DEBUG_IF2((M4OSA_ERR)M4ERR_PARAMETER == err_code, M4ERR_PARAMETER,
                                                "M4OSA_strSpan: M4OSA_chrSpan");

    *pui32_pos += ui32_length;

    return M4NO_ERROR;
}





M4OSA_ERR M4OSA_strSpanComplement(M4OSA_String str_in, M4OSA_Char* charset,
                                  M4OSA_UInt32* pui32_pos)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err_code;
    M4OSA_UInt32 ui32_length;

    M4OSA_TRACE1_3("M4OSA_strSpanComplement\t\tM4OSA_String 0x%x\tM4OSA_Char* "
        "0x%x\tM4OSA_UInt32* 0x%x", str_in, charset, pui32_pos);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER,
                                                     "M4OSA_strSpanComplement");
    M4OSA_DEBUG_IF2(M4OSA_NULL == charset, M4ERR_PARAMETER,
                                                     "M4OSA_strSpanComplement");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pui32_pos, M4ERR_PARAMETER,
                                                     "M4OSA_strSpanComplement");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                     "M4OSA_strSpanComplement");

    if(*pui32_pos >= pStr->ui32_length)
    {
        return M4ERR_STR_OVERFLOW;
    }

    err_code = M4OSA_chrSpanComplement(pStr->pui8_buffer + *pui32_pos,
                    charset, &ui32_length);

    M4OSA_DEBUG_IF2((M4OSA_ERR)M4ERR_PARAMETER == err_code, M4ERR_PARAMETER,
                "M4OSA_strSpanComplement: M4OSA_chrSpanComplement");

    if(M4WAR_CHR_NOT_FOUND == err_code)
    {
        return M4WAR_STR_NOT_FOUND;
    }

    *pui32_pos += ui32_length;

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strFindFirstChar(M4OSA_String str_in, M4OSA_Char c,
                                 M4OSA_UInt32* pui32_pos)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_Char* pBuffer;
    M4OSA_Char* pchar;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strFindFirstChar\t\tM4OSA_String 0x%x\tM4OSA_Char"
        " 0x%x\tM4OSA_UInt32* 0x%x", str_in, c, pui32_pos);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER,
                                                      "M4OSA_strFindFirstChar");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pui32_pos, M4ERR_PARAMETER,
                                                      "M4OSA_strFindFirstChar");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                      "M4OSA_strFindFirstChar");

    if(*pui32_pos >= pStr->ui32_length)
    {
        return M4ERR_STR_OVERFLOW;
    }

    pBuffer = pStr->pui8_buffer + *pui32_pos;

    err_code = M4OSA_chrFindChar(pBuffer, c, &pchar);

    M4OSA_DEBUG_IF2(err_code == (M4OSA_ERR)M4ERR_PARAMETER,
        M4ERR_PARAMETER, "M4OSA_strFindFirstChar");

    if(M4WAR_CHR_NOT_FOUND == err_code)
    {
        return M4WAR_STR_NOT_FOUND;
    }

    *pui32_pos = pchar - pStr->pui8_buffer;

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strFindLastChar(M4OSA_String str_in, M4OSA_Char c,
                                M4OSA_UInt32* pui32_pos)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_Char* pBuffer;
    M4OSA_UInt32 i;

    M4OSA_TRACE1_3("M4OSA_strFindLastChar\t\tM4OSA_String 0x%x\tM4OSA_Char"
        " 0x%x\tM4OSA_UInt32* 0x%x", str_in, c, pui32_pos);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER,"M4OSA_strFindLastChar");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pui32_pos, M4ERR_PARAMETER,
                                                       "M4OSA_strFindLastChar");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                       "M4OSA_strFindLastChar");

    if(*pui32_pos > pStr->ui32_length)
    {
        return M4ERR_STR_OVERFLOW;
    }

    pBuffer = pStr->pui8_buffer;

    for(i=(*pui32_pos); i!=0; i--)
    {
        if(pBuffer[i] == c)
        {
            *pui32_pos = i;

            return M4NO_ERROR;
        }
    }

    return M4WAR_STR_NOT_FOUND;
}


M4OSA_ERR M4OSA_strFindFirstSubStr(M4OSA_String str_in1, M4OSA_String str_in2,
                                   M4OSA_UInt32* pui32_pos)
{
    M4OSA_strStruct* pStr1 = (M4OSA_strStruct*)str_in1;
    M4OSA_strStruct* pStr2 = (M4OSA_strStruct*)str_in2;
    M4OSA_Char* pResult;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strFindFirstSubStr\t\tM4OSA_String 0x%x\tM4OSA_String"
        " 0x%x\tM4OSA_UInt32* 0x%x", str_in1, str_in2, pui32_pos);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr1, M4ERR_PARAMETER,
                                                    "M4OSA_strFindFirstSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr2, M4ERR_PARAMETER,
                                                    "M4OSA_strFindFirstSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pui32_pos, M4ERR_PARAMETER,
                                                    "M4OSA_strFindFirstSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr1->coreID, M4ERR_STR_BAD_STRING,
                                                    "M4OSA_strFindFirstSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr2->coreID, M4ERR_STR_BAD_STRING,
                                                    "M4OSA_strFindFirstSubStr");

    if(*pui32_pos >= pStr1->ui32_length)
    {
        return M4ERR_STR_OVERFLOW;
    }

    if(pStr2->ui32_length == 0)
    {
        return M4WAR_STR_NOT_FOUND;
    }

    err_code = M4OSA_chrFindPattern(pStr1->pui8_buffer + (*pui32_pos),
                pStr2->pui8_buffer, &pResult);

    M4OSA_DEBUG_IF2((M4OSA_ERR)M4ERR_PARAMETER == err_code, M4ERR_PARAMETER,
                              "M4OSA_strFindFirstSubStr: M4OSA_chrFindPattern");

    if(M4WAR_CHR_NOT_FOUND == err_code)
    {
        return M4WAR_STR_NOT_FOUND;
    }

    *pui32_pos = pResult - pStr1->pui8_buffer;

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strFindLastSubStr(M4OSA_String str_in1, M4OSA_String str_in2,
                                  M4OSA_UInt32* pui32_pos)
{
    M4OSA_strStruct* pStr1 = (M4OSA_strStruct*)str_in1;
    M4OSA_strStruct* pStr2 = (M4OSA_strStruct*)str_in2;
    M4OSA_Int32 i32_result;


    M4OSA_TRACE1_3("M4OSA_strFindLastSubStr\t\tM4OSA_String 0x%x\tM4OSA_String"
        " 0x%x\tM4OSA_UInt32* 0x%x", str_in1, str_in2, pui32_pos);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr1, M4ERR_PARAMETER,
                                                     "M4OSA_strFindLastSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr2, M4ERR_PARAMETER,
                                                     "M4OSA_strFindLastSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pui32_pos, M4ERR_PARAMETER,
                                                     "M4OSA_strFindLastSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr1->coreID, M4ERR_STR_BAD_STRING,
                                                     "M4OSA_strFindLastSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr2->coreID, M4ERR_STR_BAD_STRING,
                                                     "M4OSA_strFindLastSubStr");

    if(*pui32_pos > pStr1->ui32_length)
    {
        return M4ERR_STR_OVERFLOW;
    }

    if((pStr2->ui32_length == 0) || (pStr1->ui32_length == 0))
    {
        return M4WAR_STR_NOT_FOUND;
    }

    i32_result = M4OSA_strPrivFindLastSubStr(pStr1, pStr2, *pui32_pos);

    if(i32_result < 0)
    {
        return M4WAR_STR_NOT_FOUND;
    }

    *pui32_pos = i32_result;

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strTruncate(M4OSA_String str_in, M4OSA_UInt32 ui32_length)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;

    M4OSA_TRACE1_2("M4OSA_strTroncate\t\tM4OSA_String 0x%x\tM4OSA_UInt32 %d",
        str_in, ui32_length);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strTroncate");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                           "M4OSA_strTroncate");

    if(ui32_length >= pStr->ui32_length)
    {
        return M4WAR_STR_OVERFLOW;
    }

    pStr->ui32_length = ui32_length;

    if(pStr->pui8_buffer != M4OSA_NULL)
    {
        pStr->pui8_buffer[ui32_length] = '\0';
    }

    return M4NO_ERROR;
}

M4OSA_ERR M4OSA_strCopy(M4OSA_String str_out, M4OSA_String str_in)
{
    M4OSA_strStruct* pIstr = (M4OSA_strStruct*)str_in;
    M4OSA_strStruct* pOstr = (M4OSA_strStruct*)str_out;

    M4OSA_TRACE1_2("M4OSA_strCopy\t\tM4OSA_String 0x%x\tM4OSA_String 0x%x",
        str_in, str_out);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pIstr, M4ERR_PARAMETER, "M4OSA_strCopy");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pOstr, M4ERR_PARAMETER, "M4OSA_strCopy");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pIstr->coreID, M4ERR_STR_BAD_STRING,
                                                               "M4OSA_strCopy");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pOstr->coreID, M4ERR_STR_BAD_STRING,
                                                               "M4OSA_strCopy");

    return M4OSA_strPrivSet(pOstr, pIstr->pui8_buffer, pIstr->ui32_length);
}


M4OSA_ERR M4OSA_strCopySubStr(M4OSA_String str_out,
                              M4OSA_UInt32 ui32_pos,
                              M4OSA_String str_in,
                              M4OSA_UInt32 ui32_offset,
                              M4OSA_UInt32* ui32_num)
{
    M4OSA_strStruct *pIstr = (M4OSA_strStruct*)str_in;
    M4OSA_strStruct *pOstr = (M4OSA_strStruct*)str_out;
    M4OSA_ERR err_code = M4NO_ERROR;
    M4OSA_UInt32 ui32_length, olength;
    M4OSA_Char* pSrc;
    M4OSA_Char* pDest;


    M4OSA_TRACE1_5("M4OSA_strCopySubStr\t\tM4OSA_String 0x%x\tM4OSA_UInt32 %d"
        "\tM4OSA_String 0x%x\tM4OSA_UInt32 %d\tM4OSA_UInt32* %0x%x",
        str_out, str_in, str_out, ui32_pos, ui32_num);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pIstr, M4ERR_PARAMETER, "M4OSA_strCopySubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pOstr, M4ERR_PARAMETER, "M4OSA_strCopySubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == ui32_num, M4ERR_PARAMETER,
                                                         "M4OSA_strCopySubStr");
    M4OSA_DEBUG_IF2(*ui32_num == 0, M4ERR_PARAMETER, "M4OSA_strCopySubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pOstr->coreID, M4ERR_STR_BAD_STRING,
                                                         "M4OSA_strCopySubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pIstr->coreID, M4ERR_STR_BAD_STRING,
                                                         "M4OSA_strCopySubStr");
    M4OSA_DEBUG_IF2(ui32_pos > pOstr->ui32_length, M4ERR_STR_OVERFLOW,
                                                         "M4OSA_strCopySubStr");
    M4OSA_DEBUG_IF2(ui32_offset > pIstr->ui32_length, M4ERR_STR_OVERFLOW,
                                                         "M4OSA_strCopySubStr");

    /* Calculate there is enough char in str_in after ui32_offset */
    ui32_length = pIstr->ui32_length - ui32_offset;

    if(*ui32_num > ui32_length)
    {
        *ui32_num = ui32_length;

        err_code = M4WAR_STR_OVERFLOW;
    }

    /* Calculate the future length of str2 */
    ui32_length = ui32_pos + *ui32_num;

    olength = pOstr->ui32_length;

    if(ui32_length >= olength)
    {
        olength = ui32_length;
    }

    /* Reallocation if needed */
    if(M4OSA_strPrivReallocCopy(pOstr, olength) != M4NO_ERROR)
    {
        M4OSA_DEBUG(M4ERR_ALLOC, "M4OSA_strCopySubStr");

        return M4ERR_ALLOC;
    }

    pSrc  = pIstr->pui8_buffer + ui32_offset;
    pDest = pOstr->pui8_buffer + ui32_pos;

    M4OSA_memcpy(pDest, pSrc, *ui32_num);

    pOstr->ui32_length = olength;
    pOstr->pui8_buffer[pOstr->ui32_length] = '\0';

    return err_code;
}


M4OSA_ERR M4OSA_strConcat(M4OSA_String str_first, M4OSA_String str_second)
{
    M4OSA_strStruct* pStr1 = (M4OSA_strStruct*)str_first;
    M4OSA_strStruct* pStr2 = (M4OSA_strStruct*)str_second;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char* pBuffer;

    M4OSA_TRACE1_2("M4OSA_strConcat\t\tM4OSA_String 0x%x\tM4OSA_String 0x%x",
        str_first, str_second);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr1, M4ERR_PARAMETER, "M4OSA_strConcat");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr2, M4ERR_PARAMETER, "M4OSA_strConcat");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr1->coreID, M4ERR_STR_BAD_STRING,
                                                             "M4OSA_strConcat");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr2->coreID, M4ERR_STR_BAD_STRING,
                                                             "M4OSA_strConcat");

    if(pStr2->ui32_length == 0)
    {
        return M4NO_ERROR;
    }

    ui32_length = pStr1->ui32_length + pStr2->ui32_length;

    if(M4OSA_strPrivReallocCopy(pStr1, ui32_length) != M4NO_ERROR)
    {
        M4OSA_DEBUG(M4ERR_ALLOC, "M4OSA_strConcat");

        return M4ERR_ALLOC;
    }

    pBuffer = pStr1->pui8_buffer + pStr1->ui32_length;

    /* Fill the actual M4OSA_String content */
    M4OSA_memcpy(pBuffer, pStr2->pui8_buffer, pStr2->ui32_length+1);

    pStr1->ui32_length = ui32_length;

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strInsertSubStr(M4OSA_String str_out,
                                M4OSA_UInt32 ui32_pos,
                                M4OSA_String str_in,
                                M4OSA_UInt32 ui32_offset,
                                M4OSA_UInt32* ui32_num)
{
    M4OSA_strStruct *pIstr = (M4OSA_strStruct*)str_in;
    M4OSA_strStruct *pOstr = (M4OSA_strStruct*)str_out;
    M4OSA_ERR err_code, return_code = M4NO_ERROR;
    M4OSA_UInt32 ui32_length;

    M4OSA_TRACE1_5("M4OSA_strInsertSubStr\t\tM4OSA_String 0x%x\tM4OSA_UInt32 %d"
        "\tM4OSA_String 0x%x\tM4OSA_UInt32 %d\tM4OSA_UInt32* %0x%x",
        str_out, str_in, str_out, ui32_pos, ui32_num);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pIstr, M4ERR_PARAMETER,
                                                       "M4OSA_strInsertSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pOstr, M4ERR_PARAMETER,
                                                       "M4OSA_strInsertSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == ui32_num, M4ERR_PARAMETER,
                                                       "M4OSA_strInsertSubStr");
    M4OSA_DEBUG_IF2(*ui32_num == 0, M4ERR_PARAMETER,
                                                       "M4OSA_strInsertSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pIstr->coreID, M4ERR_STR_BAD_STRING,
                                                       "M4OSA_strInsertSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pOstr->coreID, M4ERR_STR_BAD_STRING,
                                                       "M4OSA_strInsertSubStr");
    M4OSA_DEBUG_IF2(ui32_pos > pOstr->ui32_length, M4ERR_STR_OVERFLOW,
                                                       "M4OSA_strInsertSubStr");
    M4OSA_DEBUG_IF2(ui32_offset > pIstr->ui32_length, M4ERR_STR_OVERFLOW,
                                                       "M4OSA_strInsertSubStr");

    /* Calculate there is enough char in str_in after ui32_offset */
    ui32_length = pIstr->ui32_length - ui32_offset;

    if(*ui32_num > ui32_length)
    {
        *ui32_num = ui32_length;

        return_code = M4WAR_STR_OVERFLOW;
    }

    err_code = M4OSA_strPrivSetAndRepleceStr(pOstr, ui32_pos, 0,
        pIstr->pui8_buffer + ui32_offset, *ui32_num);

    if(err_code == (M4OSA_ERR)M4ERR_ALLOC)
    {
        M4OSA_DEBUG(M4ERR_ALLOC, "M4OSA_strInsertSubStr");

        return M4ERR_ALLOC;
    }

    return return_code;
}


M4OSA_ERR M4OSA_strDelSubStr(M4OSA_String str_in, M4OSA_UInt32 ui32_offset,
                             M4OSA_UInt32* ui32_num)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_length;
    M4OSA_Char* pBuffer;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strDelSubStr\t\tM4OSA_String 0x%x\tM4OSA_UInt32 %d\t"
        "M4OSA_UInt32* 0x%x", str_in, ui32_offset, ui32_num);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strDelSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == ui32_num, M4ERR_PARAMETER,
                                                          "M4OSA_strDelSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                          "M4OSA_strDelSubStr");

    pBuffer = pStr->pui8_buffer;

    ui32_length = pStr->ui32_length ;

    if(ui32_offset >= ui32_length)
    {
        return M4ERR_STR_OVERFLOW;
    }

    ui32_length -= ui32_offset;

    if(*ui32_num >= ui32_length)
    {
        *ui32_num = ui32_length;

        pStr->ui32_length -= ui32_length;

        pBuffer[pStr->ui32_length] = '\0';

        err_code = M4WAR_STR_OVERFLOW;
    }
    else
    {
        err_code = M4OSA_strPrivSetAndRepleceStr(pStr, ui32_offset, *ui32_num,
            M4OSA_NULL, 0);
    }

    return err_code;
}


M4OSA_ERR M4OSA_strReplaceSubStr(M4OSA_String str_in, M4OSA_String str_old,
                                 M4OSA_String str_new, M4OSA_strMode mode)
{
    M4OSA_strStruct* pIstr = (M4OSA_strStruct*)str_in;
    M4OSA_strStruct* pOstr = (M4OSA_strStruct*)str_old;
    M4OSA_strStruct* pNstr = (M4OSA_strStruct*)str_new;
    M4OSA_UInt32 olength, nlength, ilength;
    M4OSA_Bool ostr2free = M4OSA_FALSE;
    M4OSA_Bool nstr2free = M4OSA_FALSE;
    M4OSA_ERR err_code;


    M4OSA_TRACE1_4("M4OSA_strReplaceSubStr\t\tM4OSA_String 0x%x\tM4OSA_String "
        "0x%x\tM4OSA_String 0x%x\tM4OSA_strSupprMode %d",
        str_in, str_old, str_new, mode);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pIstr, M4ERR_PARAMETER,
                                                      "M4OSA_strReplaceSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pOstr, M4ERR_PARAMETER,
                                                      "M4OSA_strReplaceSubStr");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pNstr, M4ERR_PARAMETER,
                                                      "M4OSA_strReplaceSubStr");
    M4OSA_DEBUG_IF2((mode != M4OSA_kstrAll) && (mode != M4OSA_kstrEnd)
        && (mode != M4OSA_kstrBegin), M4ERR_PARAMETER,
                                                      "M4OSA_strReplaceSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pIstr->coreID, M4ERR_STR_BAD_STRING,
                                                      "M4OSA_strReplaceSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pOstr->coreID, M4ERR_STR_BAD_STRING,
                                                      "M4OSA_strReplaceSubStr");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pNstr->coreID, M4ERR_STR_BAD_STRING,
                                                      "M4OSA_strReplaceSubStr");

    olength = pOstr->ui32_length;
    nlength = pNstr->ui32_length;
    ilength = pIstr->ui32_length;

    if((olength == 0) || (ilength == 0) || (olength > ilength))
    {
        M4OSA_DEBUG(M4WAR_STR_NOT_FOUND, "M4OSA_strReplaceSubStr");

        return M4WAR_STR_NOT_FOUND;
    }

    if(pIstr == pOstr)
    {
        M4OSA_strPrivDuplicate(&pOstr, pIstr);

        ostr2free = M4OSA_TRUE;
    }

    if(pIstr == pNstr)
    {
        M4OSA_strPrivDuplicate(&pNstr, pIstr);

        nstr2free = M4OSA_TRUE;
    }

    if(nlength == olength)
    {
        err_code = M4OSA_strPrivReplaceSameSizeStr(pIstr, pOstr, pNstr, mode);
    }
    else if(nlength < olength)
    {
        err_code = M4OSA_strPrivReplaceSmallerStr(pIstr, pOstr, pNstr, mode);
    }
    else
    {
        err_code = M4OSA_strPrivReplaceBiggerStr(pIstr, pOstr, pNstr, mode);
    }

    if(ostr2free == M4OSA_TRUE)
    {
        M4OSA_free((M4OSA_MemAddr32)pOstr->pui8_buffer);
        M4OSA_free((M4OSA_MemAddr32)pOstr);
    }

    if(nstr2free == M4OSA_TRUE)
    {
        M4OSA_free((M4OSA_MemAddr32)pNstr->pui8_buffer);
        M4OSA_free((M4OSA_MemAddr32)pNstr);
    }

    return err_code;
}


M4OSA_ERR M4OSA_strGetFirstToken(M4OSA_String str_in, M4OSA_String str_token,
                                 M4OSA_String str_delim)
{
    M4OSA_strStruct* pIn =    (M4OSA_strStruct*)str_in;
    M4OSA_strStruct* pToken = (M4OSA_strStruct*)str_token;
    M4OSA_strStruct* pDelim = (M4OSA_strStruct*)str_delim;
    M4OSA_UInt32 length_token, length_delim;
    M4OSA_Char* pBuffer;
    M4OSA_Char* pchar;
    M4OSA_ERR err_code;


    M4OSA_TRACE1_3("M4OSA_strGetFirstToken\t\tM4OSA_String 0x%x\tM4OSA_String"
        " 0x%x\tM4OSA_String 0x%x", str_in, str_token, str_delim);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pIn, M4ERR_PARAMETER,"M4OSA_strGetFirstToken");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pToken, M4ERR_PARAMETER,
                                                      "M4OSA_strGetFirstToken");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pDelim, M4ERR_PARAMETER,
                                                      "M4OSA_strGetFirstToken");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pIn->coreID, M4ERR_STR_BAD_STRING,
                                                      "M4OSA_strGetFirstToken");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pToken->coreID, M4ERR_STR_BAD_STRING,
                                                      "M4OSA_strGetFirstToken");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pDelim->coreID, M4ERR_STR_BAD_STRING,
                                                      "M4OSA_strGetFirstToken");

    length_delim = pDelim->ui32_length;

    if(pDelim->ui32_length == 0)
    {
        M4OSA_DEBUG(M4WAR_STR_NOT_FOUND, "M4OSA_strGetFirstToken");

        return M4WAR_STR_NOT_FOUND;
    }

    pBuffer = pIn->pui8_buffer;

    err_code = M4OSA_chrFindPattern(pBuffer,pDelim->pui8_buffer, &pchar);

    if(err_code != M4NO_ERROR)
    {
        M4OSA_DEBUG(M4WAR_STR_NOT_FOUND, "M4OSA_strGetFirstToken");

        return M4WAR_STR_NOT_FOUND;
    }

    length_token = pchar - pBuffer;

    err_code = M4OSA_strPrivSet(pToken, pBuffer, length_token);

    if(err_code == (M4OSA_ERR)M4ERR_ALLOC)
    {
        M4OSA_DEBUG(M4ERR_ALLOC, "M4OSA_strGetFirstToken: M4OSA_strPrivSet");

        return M4ERR_ALLOC;
    }

    err_code = M4OSA_strPrivSetAndRepleceStr(pIn, 0, length_token + length_delim,
        M4OSA_NULL, 0);

    if(err_code == (M4OSA_ERR)M4ERR_ALLOC)
    {
        M4OSA_DEBUG(M4ERR_ALLOC,
            "M4OSA_strGetFirstToken: M4OSA_strPrivSetAndRepleceStr");

        return M4ERR_ALLOC;
    }

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strGetLastToken(M4OSA_String str_in, M4OSA_String str_token,
                                M4OSA_String str_delim)
{
    M4OSA_strStruct* pIn =    (M4OSA_strStruct*)str_in;
    M4OSA_strStruct* pToken = (M4OSA_strStruct*)str_token;
    M4OSA_strStruct* pDelim = (M4OSA_strStruct*)str_delim;
    M4OSA_UInt32 in_length, token_length, delim_length;
    M4OSA_Char* pIn_buffer;
    M4OSA_Char* pToken_buffer;
    M4OSA_Int32 delim_pos;
    M4OSA_ERR err_code;

    M4OSA_TRACE1_3("M4OSA_strGetLastToken\t\tM4OSA_String 0x%x\tM4OSA_String"
        " 0x%x\tM4OSA_String 0x%x", str_in, str_token, str_delim);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pIn, M4ERR_PARAMETER,
                                                       "M4OSA_strGetLastToken");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pToken, M4ERR_PARAMETER,
                                                       "M4OSA_strGetLastToken");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pDelim, M4ERR_PARAMETER,
                                                       "M4OSA_strGetLastToken");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pIn->coreID, M4ERR_STR_BAD_STRING,
                                                       "M4OSA_strGetLastToken");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pToken->coreID, M4ERR_STR_BAD_STRING,
                                                       "M4OSA_strGetLastToken");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pDelim->coreID, M4ERR_STR_BAD_STRING,
                                                       "M4OSA_strGetLastToken");

    in_length = pIn->ui32_length;
    delim_length = pDelim->ui32_length;
    pIn_buffer = pIn->pui8_buffer;

    if(pDelim->ui32_length > pIn->ui32_length)
    {
        M4OSA_DEBUG(M4WAR_STR_NOT_FOUND, "M4OSA_strGetLastToken");

        return M4WAR_STR_NOT_FOUND;
    }

    delim_pos = M4OSA_strPrivFindLastSubStr(pIn, pDelim, in_length-delim_length);

    if(delim_pos < 0)
    {
        M4OSA_DEBUG(M4WAR_STR_NOT_FOUND, "M4OSA_strGetLastToken");

        return M4WAR_STR_NOT_FOUND;
    }

    pToken_buffer = pIn_buffer + delim_pos + delim_length;
    token_length = in_length - delim_pos + delim_length;

    err_code = M4OSA_strPrivSet(str_token, pToken_buffer, token_length);

    if(err_code == (M4OSA_ERR)M4ERR_ALLOC)
    {
        M4OSA_DEBUG(M4ERR_ALLOC,
            "M4OSA_strGetLastToken: M4OSA_strPrivSet");

        return err_code;
    }

    pIn_buffer[delim_pos] = '\0';

    pIn->ui32_length = delim_pos;

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strSetUpperCase(M4OSA_String str_in)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_Char* pBuffer;
    M4OSA_Char* pchar;


    M4OSA_TRACE1_1("M4OSA_strSetUpperCase\t\tM4OSA_String 0x%x", str_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetUpperCase");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                       "M4OSA_strSetUpperCase");

    pBuffer = pStr->pui8_buffer;

    for(pchar=pBuffer; pchar!=(pBuffer+pStr->ui32_length); pchar++)
    {
        *pchar = M4OSA_chrToUpper(*pchar);
    }

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strSetLowerCase(M4OSA_String str_in)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_Char* pBuffer;
    M4OSA_Char* pchar;

    M4OSA_TRACE1_1("M4OSA_strSetLowerCase\t\tM4OSA_String 0x%x", str_in);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetLowerCase");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                       "M4OSA_strSetLowerCase");

    pBuffer = pStr->pui8_buffer;

    for(pchar=pBuffer; pchar!=(pBuffer+pStr->ui32_length); pchar++)
    {
        *pchar = M4OSA_chrToLower(*pchar);
    }


    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strSprintf(M4OSA_String str_in, M4OSA_Char* pFormat, ...)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_ERR err_code = M4ERR_STR_OVERFLOW;
    M4OSA_UInt32 ui32_size;
    va_list args;

    M4OSA_TRACE1_2("M4OSA_strSprintf\t\tM4OSA_String 0x%x\tM4OSA_Char* 0x%x",
        str_in, pFormat);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pFormat, M4ERR_PARAMETER, "M4OSA_strSprintf");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSprintf");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                       "M4OSA_strSetLowerCase");

    ui32_size = pStr->ui32_length + (M4OSA_UInt32)(1.5 * M4OSA_chrLength(pFormat));

    va_start(args, pFormat);

    while(err_code == (M4OSA_ERR)M4ERR_STR_OVERFLOW)
    {
        err_code = M4OSA_strPrivReallocCopy(pStr, ui32_size);

        if(err_code == (M4OSA_ERR)M4ERR_ALLOC)
        {
            M4OSA_DEBUG(M4ERR_ALLOC, "M4OSA_strSprintf");

            va_end(args);

            return M4ERR_ALLOC;
        }

        ui32_size *= 2;

        err_code = M4OSA_strPrivSPrintf(pStr, pFormat, args);
    }

    va_end(args);

    pStr->ui32_length = M4OSA_chrLength(pStr->pui8_buffer);

    return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strSetMinAllocationSize(M4OSA_String str_in, M4OSA_UInt32 ui32_newsize)
{
    M4OSA_strStruct* pStr = (M4OSA_strStruct*)str_in;
    M4OSA_UInt32 ui32_size;
    M4OSA_Char* pBuffer;
    M4OSA_Char* pIbuffer;

    M4OSA_TRACE1_2("M4OSA_strSetMinAllocationSize\t\tM4OSA_String 0x%x\t"
        "M4OSA_Int32 %d", str_in, ui32_newsize);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pStr, M4ERR_PARAMETER, "M4OSA_strSetInt32");
    M4OSA_DEBUG_IF2(M4OSA_STRING != pStr->coreID, M4ERR_STR_BAD_STRING,
                                                           "M4OSA_strSetInt32");

    ui32_size = pStr->ui32_size;
    pIbuffer = pStr->pui8_buffer;

    if(ui32_newsize > ui32_size)
    {
        ui32_size = ui32_newsize + ((4 - (ui32_newsize % 4)) % 4);
    }

    /* Allocate the actual M4OSA_String content */
    pBuffer = (M4OSA_Char*)M4OSA_malloc(ui32_size * sizeof(M4OSA_Char),
        M4OSA_STRING, (M4OSA_Char*)"M4OSA_strSetMinAllocationSize");

    M4OSA_CHECK_MALLOC(pBuffer, "M4OSA_strSetMinAllocationSize");

    if(pIbuffer != M4OSA_NULL)
    {
        M4OSA_memcpy(pBuffer, pIbuffer, pStr->ui32_length+1);

        M4OSA_free((M4OSA_MemAddr32)pIbuffer);
    }

    pStr->pui8_buffer = pBuffer;

    pStr->ui32_size = ui32_size;

    return M4NO_ERROR;
}

