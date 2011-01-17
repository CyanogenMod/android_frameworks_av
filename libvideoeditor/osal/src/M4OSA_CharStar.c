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
 * @file         M4DPAK_CharStar.c
 * @ingroup
  * @brief        definition of the Char Star set of functions.
 * @note         This file defines the Char Star set of functions.
 *
 ************************************************************************
*/


#include "M4OSA_CharStar.h"
#include "M4OSA_Memory.h"
#include "M4OSA_Debug.h"

/* WARNING: Specific Android */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>


/**
 ************************************************************************
 * @brief      This function mimics the functionality of the libc's strncpy().
 * @note       It copies exactly len2Copy characters from pStrIn to pStrOut,
 *             truncating  pStrIn or adding null characters to pStrOut if
 *             necessary.
 *             - If len2Copy is less than or equal to the length of pStrIn,
 *               a null character is appended automatically to the copied
 *               string.
 *             - If len2Copy is greater than the length of pStrIn, pStrOut is
 *               padded with null characters up to length len2Copy.
 *             - pStrOut and pStrIn MUST NOT OVERLAP (this is NOT CHECKED).
 * @param      pStrOut: (OUT) Destination character string.
 * @param      pStrIn: (IN) Source character string.
 * @param      len2Copy: (IN) Maximum number of characters from pStrIn to copy.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pStrOut is M4OSA_NULL.
  ************************************************************************
*/
M4OSA_ERR M4OSA_chrNCopy(M4OSA_Char* pStrOut, M4OSA_Char   *pStrIn, M4OSA_UInt32 len2Copy)
{
    M4OSA_TRACE1_3("M4OSA_chrNCopy\t(M4OSA_Char* %x,M4OSA_Char* %x,M4OSA_UInt32 %ld)",
        pStrOut,pStrIn,len2Copy);
    M4OSA_DEBUG_IF2((M4OSA_NULL == pStrOut),M4ERR_PARAMETER,
                            "M4OSA_chrNCopy:\tpStrOut is M4OSA_NULL");
    M4OSA_DEBUG_IF2((M4OSA_NULL == pStrIn),M4ERR_PARAMETER,
                            "M4OSA_chrNCopy:\tpStrIn is M4OSA_NULL");

    strncpy((char *)pStrOut, (const char *)pStrIn, (size_t)len2Copy);
    if(len2Copy <= (M4OSA_UInt32)strlen((const char *)pStrIn))
    {
        pStrOut[len2Copy] = '\0';
    }

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function mimics the functionality of the libc's strncat().
 * @note       It appends at most len2Append characters from pStrIn to the end
 *             of pStrOut. The initial character of pStrIn overrides the null
 *             character at the end of pStrOut. THIS LAST NULL CHARACTER IN
 *             pStrOut MUST BE PRESENT.
 *             - If a null character appears in pStrIn before len2Append
 *               characters are appended, the function appends all characters
 *               from pStrIn, up to this M4OSA_NULL character.
 *             - If len2Append is greater than the length of pStrIn, the length
 *               of pStrIn is used in place of len2Append. The resulting string
 *               is terminated with a null character.
 *             - pStrOut and pStrIn MUST NOT OVERLAP (this is NOT CHECKED).
 * @param      pStrOut: (OUT) Destination character string.
 * @param      pStrIn: (IN) character string to append.
 * @param      len2Append: (IN) Max number of characters from pStrIn to append.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pStrOut is M4OSA_NULL.
  ************************************************************************
*/
M4OSA_ERR M4OSA_chrNCat(M4OSA_Char* pStrOut, M4OSA_Char* pStrIn,
                                                        M4OSA_UInt32 len2Append)
{
    M4OSA_TRACE1_3("M4OSA_chrNCat\t(M4OSA_Char* %x,M4OSA_Char* %x,M4OSA_UInt32 %ld)",
                            pStrOut,pStrIn,len2Append);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrOut, M4ERR_PARAMETER,
                                       "M4OSA_chrNCat:\tpStrOut is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn,M4ERR_PARAMETER,
                                        "M4OSA_chrNCat:\tpStrIn is M4OSA_NULL");

    strncat((char *)pStrOut, (const char*)pStrIn, (size_t)len2Append);

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function mimics the functionality of the libc's strcmp().
 * @note       It compares pStrIn1 and pStrIn2 lexicographically.
 *             The value returned in cmpResult is greater than, equal to, or
 *             less than 0, if the string pointed to by pStrIn1 is greater than,
 *             equal to, or less than the string pointed to by pStrIn2
 *             respectively. The sign of a non-zero return value is determined
 *             by the sign of the difference between the values of the first
 *             pair of bytes that differ in the strings being compared.
 * @param      pStrIn1: (IN) First character string.
 * @param      pStrIn2: (IN) Second character string.
 * @param      cmpResult: (OUT) Comparison result.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn1 pStrIn2 or cmpResult is M4OSA_NULL.
  ************************************************************************
*/
M4OSA_ERR M4OSA_chrCompare(M4OSA_Char* pStrIn1, M4OSA_Char* pStrIn2,
                                                        M4OSA_Int32* pCmpResult)
{
    M4OSA_TRACE1_3("M4OSA_chrCompare\t(M4OSA_Char* %x,M4OSA_Char* %x,M4OSA_Int32* %x)",
                    pStrIn1,pStrIn2,pCmpResult);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn1, M4ERR_PARAMETER,
                                     "M4OSA_chrCompare:\tstrIn1 is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn2, M4ERR_PARAMETER,
                                     "M4OSA_chrCompare:\tstrIn2 is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pCmpResult, M4ERR_PARAMETER,
                                  "M4OSA_chrCompare:\tcmpResult is M4OSA_NULL");

    *pCmpResult = (M4OSA_Int32)strcmp((const char *)pStrIn1, (const char *)pStrIn2);

    return M4NO_ERROR;
}

/**
 ************************************************************************
  * @brief      This function mimics the functionality of the libc's strncmp().
 * @note       It lexicographically compares at most the first len2Comp
 *             characters in pStrIn1 and pStrIn2.
 *             The value returned in cmpResult is greater than, equal to, or
 *             less than 0, if the first len2Comp characters of the string
 *             pointed to by pStrIn1 is greater than, equal to, or less than the
 *             first len2Comp characters of the string pointed to by pStrIn2
 *             respectively. The sign of a non-zero return value is determined
 *             by the sign of the difference between the values of the first
 *             pair of bytes that differ in the strings being compared.
 * @param      pStrIn1: (IN) First character string.
 * @param      pStrIn2: (IN) Second character string.
 * @param      len2Comp: (IN) Length used for the comparison.
 * @param      cmpResult: (OUT) Comparison result.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn1 pStrIn2 or cmpResult is M4OSA_NULL.
  ************************************************************************
*/
M4OSA_ERR M4OSA_chrNCompare(M4OSA_Char* pStrIn1,M4OSA_Char* pStrIn2,
                            M4OSA_UInt32 len2Comp, M4OSA_Int32* pCmpResult)
{
    M4OSA_TRACE1_4("M4OSA_chrNCompare\t(M4OSA_Char* %x,M4OSA_Char* %x,"
        "M4OSA_Int32 %ld, M4OSA_Int32* %x)",pStrIn1,pStrIn2,len2Comp,pCmpResult);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn1,M4ERR_PARAMETER,
                                   "M4OSA_chrNCompare:\tpStrIn1 is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn2,M4ERR_PARAMETER,
                                   "M4OSA_chrNCompare:\tpStrIn2 is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pCmpResult,M4ERR_PARAMETER,
                                "M4OSA_chrNCompare:\tpCmpResult is M4OSA_NULL");

    *pCmpResult = (M4OSA_Int32)strncmp((const char*)pStrIn1, (const char*)pStrIn2,
                                                              (size_t)len2Comp);

    return M4NO_ERROR;
}

/**
 ************************************************************************
  * @brief      This function returns the boolean comparison of pStrIn1 and pStrIn2.
 * @note       The value returned in result is M4OSA_TRUE if the string
 *             pointed to by pStrIn1 is strictly identical to the string pointed
 *             to by pStrIn2, and M4OSA_FALSE otherwise.
 * @param      pStrIn1: (IN) First character string.
 * @param      pStrIn2: (IN) Second character string.
 * @param      cmpResult: (OUT) Comparison result.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn1 pStrIn2 or cmpResult is M4OSA_NULL.
  ************************************************************************
*/
M4OSA_ERR M4OSA_chrAreIdentical(M4OSA_Char* pStrIn1, M4OSA_Char* pStrIn2,
                                                            M4OSA_Bool* pResult)
{
    M4OSA_UInt32 i32,len32;
    M4OSA_TRACE1_3("M4OSA_chrAreIdentical\t(M4OSA_Char* %x,M4OSA_Char* %x,"
        "M4OSA_Int32* %x)",pStrIn1,pStrIn2,pResult);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn1, M4ERR_PARAMETER,
                               "M4OSA_chrAreIdentical:\tpStrIn1 is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn2, M4ERR_PARAMETER,
                               "M4OSA_chrAreIdentical:\tpStrIn2 is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pResult, M4ERR_PARAMETER,
                               "M4OSA_chrAreIdentical:\tpResult is M4OSA_NULL");

    len32 = (M4OSA_UInt32)strlen((const char *)pStrIn1);
    if(len32 != (M4OSA_UInt32)strlen((const char *)pStrIn2))
    {
        *pResult = M4OSA_FALSE;
        return M4NO_ERROR;
    }

    for(i32=0;i32<len32;i32++)
    {
        if(pStrIn1[i32] != pStrIn2[i32])
        {
            *pResult = M4OSA_FALSE;
            return M4NO_ERROR;
        }
    }

    *pResult = M4OSA_TRUE;

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function mimics the functionality of the libc's strchr().
 * @note       It finds the first occurrence (i.e. starting from the beginning
 *             of the string) of c in pStrIn and set *pPointerInStr to this
 *             position.
 *             If no occurrence is found, *pPointerInStr is set to M4OSA_NULL.
 * @param      pStrIn: (IN) Character string where to search.
 * @param      c: (IN) Character to search.
 * @param      pPointerInStr: (OUT) pointer on the first occurrence of c.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pPointerInStr is M4OSA_NULL.
 * @return     M4WAR_CHR_NOT_FOUND: no occurrence of c found.
  ************************************************************************
*/
M4OSA_ERR M4OSA_chrFindChar (M4OSA_Char* pStrIn, M4OSA_Char c,
                                                            M4OSA_Char** pInStr)
{
    M4OSA_TRACE1_3("M4OSA_chrFindChar\t(M4OSA_Char* %x, M4OSA_Char %c"
        "M4OSA_Char** %x)",pStrIn,c,pInStr);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn,M4ERR_PARAMETER,
                                    "M4OSA_chrFindChar:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pInStr,M4ERR_PARAMETER,
                                    "M4OSA_chrFindChar:\tpInStr is M4OSA_NULL");

    *pInStr = (M4OSA_Char*)strchr((const char *)pStrIn,(int)c);
    if(M4OSA_NULL == *pInStr)
    {
        return M4WAR_CHR_NOT_FOUND;
    }
    else
    {
        return M4NO_ERROR;
    }
}

/**
 ************************************************************************
 * @brief      This function mimics the functionality of the libc's strrchr().
 * @note       It finds the last occurrence (i.e. starting from the end of the
 *             string, backward) of c in pStrIn and set *pPointerInStr to this
 *             position.
 *             If no occurrence is found, *pPointerInStr is set to M4OSA_NULL.
 * @param      pStrIn: (IN) Character string where to search.
 * @param      c: (IN) Character to search.
 * @param      pPointerInStr: (OUT) pointer on the first occurrence of c.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pPointerInStr is M4OSA_NULL.
 * @return     M4WAR_CHR_NOT_FOUND: no occurrence of c found.
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrReverseFindChar(M4OSA_Char* pStrIn, M4OSA_Char c,M4OSA_Char** pInStr)
{
    M4OSA_TRACE1_3("M4OSA_chrReverseFindChar\t(M4OSA_Char* %x, M4OSA_Char %c"
                        "M4OSA_Char** %x)",pStrIn,c,pInStr);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn, M4ERR_PARAMETER,
                             "M4OSA_chrReverseFindChar:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pInStr, M4ERR_PARAMETER,
                             "M4OSA_chrReverseFindChar:\tpInStr is M4OSA_NULL");

    *pInStr = (M4OSA_Char*)strrchr((const char *)pStrIn,(int)c);
    if(M4OSA_NULL == *pInStr)
    {
        return M4WAR_CHR_NOT_FOUND;
    }
    else
    {
        return M4NO_ERROR;
    }
}

/**
 ************************************************************************
 * @brief      This function mimics the functionality of the libc's strspn().
 * @note       It returns the length of the initial segment of string pStrIn
 *             that consists entirely of characters from string pDelimiters
 *             (it "spans" this set of characters).
 *             If no occurrence of any character present in pDelimiters is found
 *             at the beginning of pStrIn, *pPosInStr is M4OSA_NULL.
 * @param      pStrIn: (IN) Character string where to search.
 * @param      pDelimiters: (IN) Character string containing the set of
 *             characters to search.
 * @param      pPosInStr: (OUT) Length of the initial segment.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn, pDelimiters or pPosInStr is M4OSA_NULL.
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrSpan(M4OSA_Char* pStrIn,M4OSA_Char* pDelimiters,
                                                        M4OSA_UInt32* pPosInStr)
{
    M4OSA_TRACE1_3("M4OSA_chrSpan\t(M4OSA_Char* %x,M4OSA_Char* %x"
        "M4OSA_UInt32* %x)",pStrIn,pDelimiters,pPosInStr);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn, M4ERR_PARAMETER,
                                        "M4OSA_chrSpan:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pDelimiters, M4ERR_PARAMETER,
                                   "M4OSA_chrSpan:\tpDelimiters is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pPosInStr, M4ERR_PARAMETER,
                                     "M4OSA_chrSpan:\tpPosInStr is M4OSA_NULL");

    *pPosInStr = (M4OSA_UInt32)strspn((const char *)pStrIn,
                                                     (const char *)pDelimiters);

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function mimics the functionality of the libc's strcspn().
 * @note       It returns the length of the initial segment of string pStrIn
 *             that consists entirely of characters NOT from string delimiters
 *             (it spans the complement of this set of characters).
 *             If no occurrence of any character present in delimiters is found
 *             in pStrIn, *pPosInStr is set to the length of pStrIn.
 * @param      pStrIn: (IN) Character string where to search.
 * @param      delimiters: (IN) Character string containing the set of
 *             characters to search.
 * @param      pPosInStr: (OUT) Length of the initial segment.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn, delimiters or pPosInStr is M4OSA_NULL.
 * @return     M4WAR_CHR_NOT_FOUND: no occurrence of any character present in
 *             delimiters has been found in pStrIn.
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrSpanComplement (M4OSA_Char* pStrIn, M4OSA_Char* pDelimiters,
                                                        M4OSA_UInt32* pPosInStr)
{
    M4OSA_TRACE1_3("M4OSA_chrSpanComplement\t(M4OSA_Char* %x,M4OSA_Char* %x"
        "M4OSA_UInt32* %x)",pStrIn,pDelimiters,pPosInStr);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn,M4ERR_PARAMETER,
                              "M4OSA_chrSpanComplement:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pDelimiters,M4ERR_PARAMETER,
                         "M4OSA_chrSpanComplement:\tpDelimiters is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pPosInStr,M4ERR_PARAMETER,
                           "M4OSA_chrSpanComplement:\tpPosInStr is M4OSA_NULL");

    *pPosInStr = (M4OSA_UInt32)strcspn((const char *)pStrIn,
                                                     (const char *)pDelimiters);
    if(*pPosInStr < (M4OSA_UInt32)strlen((const char *)pStrIn))
    {
        return M4NO_ERROR;
    }
    else
    {
        return M4WAR_CHR_NOT_FOUND;
    }
}

/**
 ************************************************************************
 * @brief      This function mimics the functionality of the libc's strpbrk().
 * @note       It returns a pointer to the first occurrence in string pStrIn
 *             of any character from string pDelimiters, or a null pointer if
 *             no character from pDelimiters exists in pStrIn. In the latter
 *             case, WAR_NO_FOUND is returned.
 * @param      pStrIn: (IN) Character string where to search.
 * @param      pDelimiters: (IN) Character string containing the set of
 *             characters to search.
 * @param      pPointerInStr: (OUT) Pointer on the first character belonging to
 *             pDelimiters.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn, pDelimiters or pPosInStr is M4OSA_NULL.
 * @return     M4WAR_CHR_NOT_FOUND: no occurrence of any character present in
 *             pDelimiters has been found in pStrIn.
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrPbrk(M4OSA_Char* pStrIn, M4OSA_Char* pDelimiters,
                                                     M4OSA_Char **pPointerInStr)
{
    M4OSA_TRACE1_3("M4OSA_chrPbrk\t(M4OSA_Char* %x,M4OSA_Char* %x"
        "M4OSA_Char** %x)",pStrIn,pDelimiters,pPointerInStr);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn,M4ERR_PARAMETER,
                              "M4OSA_chrSpanComplement:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pDelimiters,M4ERR_PARAMETER,
                         "M4OSA_chrSpanComplement:\tpDelimiters is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pPointerInStr,M4ERR_PARAMETER,
                       "M4OSA_chrSpanComplement:\tpPointerInStr is M4OSA_NULL");

    *pPointerInStr = (M4OSA_Char*)strpbrk((const char *)pStrIn,
                                                     (const char *)pDelimiters);
    if(M4OSA_NULL == *pPointerInStr)
    {
        return M4WAR_CHR_NOT_FOUND;
    }
    else
    {
        return M4NO_ERROR;
    }
}

/**
 ************************************************************************
 * @brief      This function mimics the functionality of the libc's strstr().
 * @note       It locates the first occurrence of the string pStrIn2 (excluding
 *             the terminating null character) in string pStrIn1 and set
 *             pPointerInStr1 to the located string, or to a null pointer if the
 *             string is not found, in which case M4WAR_CHR_NOT_FOUND is
 *             returned. If pStrIn2 points to a string with zero length (that
 *             is, the string ""), the function returns pStrIn1.
 * @param      pStrIn1: (IN) Character string where to search.
 * @param      pStrIn2: (IN) Character string to search.
 * @param      pPointerInStr1: (OUT) Pointer on the first character of pStrIn2
 *             in pStrIn1.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn1, pStrIn2 or pPointerInStr1 is M4OSA_NULL.
 * @return     M4WAR_CHR_NOT_FOUND: no occurrence of pStrIn2 has been found in
 *             pStrIn1.
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrFindPattern(M4OSA_Char* pStrIn1, M4OSA_Char* pStrIn2,
                                                    M4OSA_Char** pPointerInStr1)
{
    M4OSA_TRACE1_3("M4OSA_chrFindPattern\t(M4OSA_Char* %x,M4OSA_Char* %x"
        "M4OSA_Char** %x)",pStrIn1,pStrIn2,pPointerInStr1);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn1,M4ERR_PARAMETER,
                                "M4OSA_chrFindPattern:\tpStrIn1 is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn2,M4ERR_PARAMETER,
                                "M4OSA_chrFindPattern:\tpStrIn2 is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pPointerInStr1,M4ERR_PARAMETER,
                         "M4OSA_chrFindPattern:\tpPointerInStr1 is M4OSA_NULL");

    *pPointerInStr1 = (M4OSA_Char*)strstr((const char *)pStrIn1,
                                                         (const char *)pStrIn2);
    if(M4OSA_NULL == *pPointerInStr1)
    {
        return M4WAR_CHR_NOT_FOUND;
    }
    else
    {
        return M4NO_ERROR;
    }
}

/**
 ************************************************************************
 * @brief      This function mimics the functionality of the libc's strlen().
 * @note       It returns the number of characters in pStrIn, not including
 *             the terminating null character.
 *             This function have no return code. It does not check that pStrIn
 *             does not point to null, nor is a valid character string (i.e.
 *             null-terminated).
 * @param      pStrIn: (IN) Character string.
 * @return     number of characters in pStrIn.
 ************************************************************************
*/
M4OSA_UInt32 M4OSA_chrLength(M4OSA_Char* pStrIn)
{
    M4OSA_TRACE1_1("M4OSA_chrLength\t(M4OSA_Char* %x)",pStrIn);
    return (M4OSA_UInt32)strlen((const char *)pStrIn);
}

/**
 ************************************************************************
 * @brief      This function mimics the functionality of the libc's tolower().
 * @note       It converts the character to lower case, if possible and
 *             appropriate, and returns it.
 * @param      cIn: (IN) Input character to convert.
 * @return     converted character.
 ************************************************************************
*/
M4OSA_Char M4OSA_chrToLower (M4OSA_Char cIn)
{
    M4OSA_TRACE1_1("M4OSA_chrToLower\t(M4OSA_Char %c)",cIn);
    return (M4OSA_Char)tolower((int)cIn);
}

/**
 ************************************************************************
 * @brief      This function mimics the functionality of the libc's toupper().
 * @note       It converts the character to upper case, if possible and
 *             appropriate, and returns it.
 * @param      cIn: (IN) Input character to convert.
 * @return     converted character.
 ************************************************************************
*/
M4OSA_Char M4OSA_chrToUpper(M4OSA_Char cIn)
{
    M4OSA_TRACE1_1("M4OSA_chrToUpper\t(M4OSA_Char %c)",cIn);
    return (M4OSA_Char)toupper((int)cIn);
}


M4OSA_ERR M4OSA_chrGetWord(M4OSA_Char*    pStrIn,
                           M4OSA_Char*    pBeginDelimiters,
                           M4OSA_Char*    pEndDelimiters,
                           M4OSA_Char*    pStrOut,
                           M4OSA_UInt32*pStrOutMaxLen,
                           M4OSA_Char** pOutputPointer)
{
    M4OSA_Char   *pTemp;
    M4OSA_UInt32  pos;
    M4OSA_ERR     errorCode;

    M4OSA_TRACE1_4("M4OSA_chrGetWord\t(M4OSA_Char* %x,...,...,M4OSA_Char* %x,"
        "M4OSA_UInt32* %x,M4OSA_Char** %x)",
                                   pStrIn,pStrOut,pStrOutMaxLen,pOutputPointer);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn, M4ERR_PARAMETER,
                                     "M4OSA_chrGetWord:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pBeginDelimiters, M4ERR_PARAMETER,
                            "M4OSA_chrGetWord:\tbeginDelimiters is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pEndDelimiters, M4ERR_PARAMETER,
                              "M4OSA_chrGetWord:\tendDelimiters is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrOut, M4ERR_PARAMETER,
                                    "M4OSA_chrGetWord:\tpStrOut is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrOutMaxLen, M4ERR_PARAMETER,
                               "M4OSA_chrGetWord:\tstrOutMaxLen is M4OSA_NULL");

    errorCode = M4OSA_chrSpan(pStrIn, pBeginDelimiters, &pos);
    pTemp     = pStrIn + pos;
    errorCode = M4OSA_chrSpanComplement(pTemp, pEndDelimiters, &pos);
    if(pos > *pStrOutMaxLen)
    {
        *pStrOutMaxLen = pos;
        return M4ERR_CHR_STR_OVERFLOW;
    }
    if(pos)
    {
        M4OSA_memcpy((M4OSA_MemAddr8)pStrOut,(M4OSA_MemAddr8)pTemp, pos);
    }
    pStrOut[pos]   = '\0';
    if(M4OSA_NULL != pOutputPointer)
    {
        *pOutputPointer = pTemp + pos;
    }
    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function gets a M4OSA_UInt32 from string.
 * @note       This function converts the first set of non-whitespace
 *             characters of pStrIn to a M4OSA_UInt32 value pVal, assuming a
 *             representation in base provided by the parameter base. pStrOut is
 *             set to the first character of the string following the last
 *             character of the number that has been converted.
 *             - in case of a failure during the conversion, pStrOut is not
 *               updated, and pVal is set to null.
 *             - in case of negative number, pStrOut is not updated, and pVal is
 *               set to null.
 *             - in case of numerical overflow, pVal is set to M4OSA_UINT32_MAX.
 *             - if pStrOut is not to be used, it can be set to M4OSA_NULL.
 * @param      pStrIn: (IN) Character string.
 * @param      pVal: (OUT) read value.
 * @param      pStrOut: (OUT) Output character string.
 * @param      base: (IN) Base of the character string representation.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pVal is M4OSA_NULL.
 * @return     M4ERR_CHR_CONV_FAILED: conversion failure.
 * @return     M4WAR_CHR_NUM_RANGE: the character string represents a number
 *             greater than M4OSA_UINT32_MAX.
 * @return     M4WAR_CHR_NEGATIVE: the character string represents a negative
 *             number.
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrGetUInt32(M4OSA_Char*    pStrIn,
                             M4OSA_UInt32*    pVal,
                             M4OSA_Char**    pStrOut,
                             M4OSA_chrNumBase base)
{
    M4OSA_UInt32 ul;
    char*        pTemp;

    M4OSA_TRACE1_4("M4OSA_chrGetUInt32\t(M4OSA_Char* %x, M4OSA_UInt32* %x"
        "M4OSA_Char** %x,M4OSA_chrNumBase %d)",pStrIn,pVal,pStrOut,base);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn, M4ERR_PARAMETER,
                                   "M4OSA_chrGetUInt32:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pVal, M4ERR_PARAMETER,
                                     "M4OSA_chrGetUInt32:\tpVal is M4OSA_NULL");

    errno = 0;
    switch(base)
    {
    case M4OSA_kchrDec:
        ul = strtoul((const char *)pStrIn, &pTemp, 10);
        break;
    case M4OSA_kchrHexa:
        ul = strtoul((const char *)pStrIn, &pTemp,16);
        break;
    case M4OSA_kchrOct:
        ul = strtoul((const char *)pStrIn, &pTemp,8);
        break;
    default:
        return M4ERR_PARAMETER;
    }

    /* has conversion failed ? */
    if((M4OSA_Char*)pTemp == pStrIn)
    {
        *pVal = 0;
        return M4ERR_CHR_CONV_FAILED;
    }

    /* was the number negative ? */
    if(*(pStrIn+strspn((const char *)pStrIn," \t")) == '-')
    {
        *pVal = 0;
        return M4WAR_CHR_NEGATIVE;
    }

    /* has an overflow occured ? */
    if(errno == ERANGE)
    {
        *pVal = M4OSA_UINT32_MAX;
        if(M4OSA_NULL != pStrOut)
        {
            *pStrOut = (M4OSA_Char*)pTemp;
        }
        return M4WAR_CHR_NUM_RANGE;
    }

    /* nominal case */
    *pVal = (M4OSA_UInt32)ul;
    if(M4OSA_NULL != pStrOut)
    {
        *pStrOut = (M4OSA_Char*)pTemp;
    }

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function gets a M4OSA_UInt16 from string.
 * @note       This function converts the first set of non-whitespace
 *             characters of pStrIn to a M4OSA_UInt16 value pVal, assuming a
 *             representation in base provided by the parameter base. pStrOut is
 *             set to the first character of the string following the last
 *             character of the number that has been converted.
 *             - in case of a failure during the conversion, pStrOut is not
 *               updated, and pVal is set to null.
 *             - in case of negative number, pStrOut is not updated, and pVal is
 *               set to null.
 *             - in case of numerical overflow, pVal is set to M4OSA_UINT16_MAX.
 *             - if pStrOut is not to be used, it can be set to M4OSA_NULL.
 * @param      pStrIn: (IN) Character string.
 * @param      pVal: (OUT) read value.
 * @param      pStrOut: (OUT) Output character string.
 * @param      base: (IN) Base of the character string representation.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pVal is M4OSA_NULL.
 * @return     M4ERR_CHR_CONV_FAILED: conversion failure.
 * @return     M4WAR_CHR_NUM_RANGE: the character string represents a number
 *             greater than M4OSA_UINT16_MAX.
 * @return     M4WAR_CHR_NEGATIVE: the character string represents a negative
 *             number.
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrGetUInt16 (M4OSA_Char* pStrIn, M4OSA_UInt16 *pVal,
                              M4OSA_Char** pStrOut, M4OSA_chrNumBase base)
{
    M4OSA_UInt32 ul;
    char*        pTemp;

    M4OSA_TRACE1_4("M4OSA_chrGetUInt16\t(M4OSA_Char* %x, M4OSA_UInt16* %x"
        "M4OSA_Char** %x,M4OSA_chrNumBase %d)",pStrIn,pVal,pStrOut,base);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn,M4ERR_PARAMETER,
                                   "M4OSA_chrGetUInt16:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pVal, M4ERR_PARAMETER,
                                     "M4OSA_chrGetUInt16:\tpVal is M4OSA_NULL");

    switch(base)
    {
    case M4OSA_kchrDec:
        ul = strtoul((const char *)pStrIn, &pTemp,10);
        break;
    case M4OSA_kchrHexa:
        ul = strtoul((const char *)pStrIn, &pTemp,16);
        break;
    case M4OSA_kchrOct:
        ul = strtoul((const char *)pStrIn, &pTemp,8);
        break;
    default:
        return M4ERR_PARAMETER;
    }

    /* has conversion failed ? */
    if((M4OSA_Char*)pTemp == pStrIn)
    {
        *pVal = 0;
        return M4ERR_CHR_CONV_FAILED;
    }

    /* was the number negative ? */
    if(*(pStrIn+strspn((const char *)pStrIn," \t")) == '-')
    {
        *pVal = 0;
        return M4WAR_CHR_NEGATIVE;
    }

    /* has an overflow occured ? */
    if(ul>M4OSA_UINT16_MAX)
    {
        *pVal = M4OSA_UINT16_MAX;
        if(M4OSA_NULL != pStrOut)
        {
            *pStrOut = (M4OSA_Char*)pTemp;
        }
        return M4WAR_CHR_NUM_RANGE;
    }

    /* nominal case */
    *pVal = (M4OSA_UInt16)ul;
    if(M4OSA_NULL != pStrOut)
    {
        *pStrOut = (M4OSA_Char*)pTemp;
    }
    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function gets a M4OSA_UInt8 from string.
 * @note       This function converts the first set of non-whitespace
 *             characters of pStrIn to a M4OSA_UInt8 value pVal, assuming a
 *             representation in base provided by the parameter base. pStrOut is
 *             set to the first character of the string following the last
 *             character of the number that has been converted.
 *             - in case of a failure during the conversion, pStrOut is not
 *               updated, and pVal is set to null.
 *             - in case of negative number, pStrOut is not updated, and pVal is
 *               set to null.
 *             - in case of numerical overflow, pVal is set to M4OSA_UINT8_MAX.
 *             - if pStrOut is not to be used, it can be set to M4OSA_NULL.
 * @param      pStrIn: (IN) Character string.
 * @param      pVal: (OUT) read value.
 * @param      pStrOut: (OUT) Output character string.
 * @param      base: (IN) Base of the character string representation.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pVal is M4OSA_NULL.
 * @return     M4ERR_CHR_CONV_FAILED: conversion failure.
 * @return     M4WAR_CHR_NUM_RANGE: the character string represents a number
 *             greater than M4OSA_UINT8_MAX.
 * @return     M4WAR_CHR_NEGATIVE: the character string represents a negative
 *             number.
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrGetUInt8(M4OSA_Char*        pStrIn,
                            M4OSA_UInt8*    pVal,
                            M4OSA_Char**    pStrOut,
                            M4OSA_chrNumBase base)
{
    M4OSA_UInt32 ul;
    char*        pTemp;

    M4OSA_TRACE1_4("M4OSA_chrGetUInt8\t(M4OSA_Char* %x, M4OSA_UInt8* %x"
        "M4OSA_Char** %x,M4OSA_chrNumBase %d)",pStrIn,pVal,pStrOut,base);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn, M4ERR_PARAMETER,
                                    "M4OSA_chrGetUInt8:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pVal, M4ERR_PARAMETER,
                                      "M4OSA_chrGetUInt8:\tpVal is M4OSA_NULL");

    switch(base)
    {
    case M4OSA_kchrDec:
        ul = strtoul((const char *)pStrIn, &pTemp, 10);
        break;
    case M4OSA_kchrHexa:
        ul = strtoul((const char *)pStrIn, &pTemp, 16);
        break;
    case M4OSA_kchrOct:
        ul = strtoul((const char *)pStrIn, &pTemp, 8);
        break;
    default:
        return M4ERR_PARAMETER;
    }

    /* has conversion failed ? */
    if((M4OSA_Char*)pTemp == pStrIn)
    {
        *pVal = 0;
        return M4ERR_CHR_CONV_FAILED;
    }

    /* was the number negative ? */
    if(*(pStrIn+strspn((const char *)pStrIn," \t")) == '-')
    {
        *pVal = 0;
        return M4WAR_CHR_NEGATIVE;
    }

    /* has an overflow occured ? */
    if(ul>M4OSA_UINT8_MAX)
    {
        *pVal = M4OSA_UINT8_MAX;
        if(M4OSA_NULL != pStrOut)
        {
            *pStrOut = (M4OSA_Char*)pTemp;
        }
        return M4WAR_CHR_NUM_RANGE;
    }

    /* nominal case */
    *pVal = (M4OSA_UInt8)ul;
    if(M4OSA_NULL != pStrOut)
    {
        *pStrOut = (M4OSA_Char*)pTemp;
    }
    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function gets a M4OSA_Int64 from string.
 * @note       This function converts the first set of non-whitespace
 *             characters of pStrIn to a M4OSA_Int64 value pVal, assuming a
 *             decimal representation. pStrOut is set to the first character of
 *             the string following the last character of the number that has
 *             been converted.
 *             - in case of a failure during the conversion, pStrOut is not
 *               updated, and pVal is set to null.
 *             - in case of numerical overflow or underflow, pVal is set to
 *               M4OSA_INT64_MAX or M4OSA_INT64_MIN respectively.
 *             - if pStrOut is not to be used, it can be set to M4OSA_NULL.
 * @param      pStrIn: (IN) Character string.
 * @param      pVal: (OUT) read value.
 * @param      pStrOut: (OUT) Output character string.
 * @param      base: (IN) Base of the character string representation.
 *             FOR THE MOMENT, ONLY DECIMAL REPRESENTATION IS HANDLED.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pVal is M4OSA_NULL.
 * @return     M4ERR_CHR_CONV_FAILED: conversion failure.
 * @return     M4WAR_CHR_NUM_RANGE: the character string represents a number
 *             greater than M4OSA_INT64_MAX or less than M4OSA_INT64_MIN
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrGetInt64(M4OSA_Char* pStrIn, M4OSA_Int64* pVal,
                            M4OSA_Char** pStrOut, M4OSA_chrNumBase base)
{
#ifdef M4OSA_64BITS_SUPPORTED
    M4OSA_Int64     maxVal   =  M4OSA_INT64_MAX; /* this is 2^63-1 */
    M4OSA_Int64     minVal   =  M4OSA_INT64_MIN; /* this is -2^63+1 */
    M4OSA_Char   maxStr[] =  "9223372036854775807";
    M4OSA_Char*  beginNum;
    M4OSA_UInt32 maxLen   = strlen((const char *)maxStr);
    M4OSA_UInt32 chrCount = 0;
    M4OSA_UInt8  negative = 0;

    M4OSA_TRACE1_4((M4OSA_Char *)"M4OSA_chrGetInt64\t(M4OSA_Char* %x, M4OSA_UInt64* %x"
        "M4OSA_Char** %x,M4OSA_chrNumBase %d)",pStrIn,pVal,pStrOut,base);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn, M4ERR_PARAMETER,
                                    "M4OSA_chrGetInt64:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pVal, M4ERR_PARAMETER,
                                      "M4OSA_chrGetInt64:\tpVal is M4OSA_NULL");

    switch(base)
    {
    case M4OSA_kchrDec:
        break;
    case M4OSA_kchrOct:
        return M4ERR_NOT_IMPLEMENTED;
    case M4OSA_kchrHexa:
        return M4ERR_NOT_IMPLEMENTED;
    default:
        return M4ERR_PARAMETER;
    }

    /* trim blank characters */
    while (*pStrIn == ' ' || *pStrIn == '\t') pStrIn++;

    /* get the sign */
    if (*pStrIn == '+') pStrIn++;
    else if (*pStrIn == '-')
    {
        negative = 1;
        pStrIn++;
    }
    beginNum = pStrIn;

    /* get the length of the numerical part */
    while((*pStrIn >= '0') && (*pStrIn <= '9'))
    {
        pStrIn++;
        chrCount++;
    }

    /* has conversion failed ? */
    if(!chrCount)
    {
        *pVal = 0;
        return M4ERR_CHR_CONV_FAILED;
    }

    /* has overflow (or underflow) occured ? */
    if((chrCount > maxLen) /* obvious overflow (or underflow) */
        ||
        ((chrCount == maxLen) && (strncmp((const char *)beginNum,
                                           (const char *)maxStr, maxLen) > 0)))
        /* less obvious overflow (or underflow) */
    {
        if(negative)
        {
            *pVal = minVal;
        }
        else
        {
            *pVal = maxVal;
        }
        if(M4OSA_NULL != pStrOut)
        {
            *pStrOut = beginNum+chrCount;
        }
        return M4WAR_CHR_NUM_RANGE;
    }

    /* nominal case */
    pStrIn = beginNum;
    *pVal  = 0;
    while((*pStrIn >= '0') && (*pStrIn <= '9'))
    {
        *pVal = (*pVal)*10 + (*pStrIn++ - '0');
    }
    if(negative)
    {
        *pVal = -*pVal;
    }
    if(M4OSA_NULL != pStrOut)
    {
        *pStrOut = pStrIn;
    }
    return M4NO_ERROR;
#elif defined M4OSA_64BITS_NOT_SUPPORTED
    return(M4OSA_chrGetInt32(pStrIn, (M4OSA_Int32*) pVal, pStrOut, M4OSA_kchrDec));
#else
    return(M4ERR_NOT_IMPLEMENTED);
#endif

                                   }

/**
 ************************************************************************
 * @brief      This function gets a M4OSA_Int32 from string.
 * @note       This function converts the first set of non-whitespace
 *             characters of pStrIn to a M4OSA_Int32 value pVal, assuming a
 *             representation in base provided by the parameter base. pStrOut is
 *             set to the first character of the string following the last
 *             character of the number that has been converted.
 *             - in case of a failure during the conversion, pStrOut is not
 *               updated, and pVal is set to null.
 *             - in case of numerical overflow or underflow, pVal is set to
 *               M4OSA_INT32_MAX or M4OSA_INT32_MIN respectively.
 *             - if pStrOut is not to be used, it can be set to M4OSA_NULL.
 * @param      pStrIn: (IN) Character string.
 * @param      pVal: (OUT) read value.
 * @param      pStrOut: (OUT) Output character string.
 * @param      base: (IN) Base of the character string representation.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pVal is M4OSA_NULL.
 * @return     M4ERR_CHR_CONV_FAILED: conversion failure.
 * @return     M4WAR_CHR_NUM_RANGE: the character string represents a number
 *             greater than M4OSA_INT32_MAX or less than M4OSA_INT32_MIN
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrGetInt32(M4OSA_Char*        pStrIn,
                            M4OSA_Int32*    pVal,
                            M4OSA_Char**    pStrOut,
                            M4OSA_chrNumBase base)
{
    M4OSA_Int32 l;
    char*       pTemp;

    M4OSA_TRACE1_4("M4OSA_chrGetInt32\t(M4OSA_Char* %x, M4OSA_Int32* %x"
        "M4OSA_Char** %x,M4OSA_chrNumBase %d)",pStrIn,pVal,pStrOut,base);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn,M4ERR_PARAMETER,
                                    "M4OSA_chrGetInt32:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pVal,M4ERR_PARAMETER,
                                      "M4OSA_chrGetInt32:\tpVal is M4OSA_NULL");

    errno = 0;
    switch(base)
    {
    case M4OSA_kchrDec:
        l = strtol((const char *)pStrIn, &pTemp, 10);
        break;
    case M4OSA_kchrHexa:
        l = strtol((const char *)pStrIn, &pTemp, 16);
        break;
    case M4OSA_kchrOct:
        l = strtol((const char *)pStrIn, &pTemp, 8);
        break;
    default:
        return M4ERR_PARAMETER;
    }

    /* has conversion failed ? */
    if((M4OSA_Char*)pTemp == pStrIn)
    {
        *pVal = 0;
        return M4ERR_CHR_CONV_FAILED;
    }

    /* has an overflow occured ? */
    if((errno == ERANGE) && (l == M4OSA_INT32_MAX))
    {
        *pVal = M4OSA_INT32_MAX;
        if(M4OSA_NULL != pStrOut)
        {
            *pStrOut = (M4OSA_Char*)pTemp;
        }
        return M4WAR_CHR_NUM_RANGE;
    }

    /* has an underflow occured ? */
    if((errno == ERANGE) && (l ==  M4OSA_INT32_MIN))
    {
        *pVal = M4OSA_INT32_MIN;
        if(M4OSA_NULL != pStrOut)
        {
            *pStrOut = (M4OSA_Char*)pTemp;
        }
        return M4WAR_CHR_NUM_RANGE;
    }

    /* nominal case */
    *pVal = (M4OSA_Int32)l;
    if(M4OSA_NULL != pStrOut)
    {
        *pStrOut = (M4OSA_Char*)pTemp;
    }

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function gets a M4OSA_Int16 from string.
 * @note       This function converts the first set of non-whitespace
 *             characters of pStrIn to a M4OSA_Int16 value pVal, assuming a
 *             representation in base provided by the parameter base. pStrOut is
 *             set to the first character of the string following the last
 *             character of the number that has been converted.
 *             - in case of a failure during the conversion, pStrOut is not
 *               updated, and pVal is set to null.
 *             - in case of numerical overflow or underflow, pVal is set to
 *               M4OSA_INT16_MAX or M4OSA_INT16_MIN respectively.
 *             - if pStrOut is not to be used, it can be set to M4OSA_NULL.
 * @param      pStrIn: (IN) Character string.
 * @param      pVal: (OUT) read value.
 * @param      pStrOut: (OUT) Output character string.
 * @param      base: (IN) Base of the character string representation.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pVal is M4OSA_NULL.
 * @return     M4ERR_CHR_CONV_FAILED: conversion failure.
 * @return     M4WAR_CHR_NUM_RANGE: the character string represents a number
 *             greater than M4OSA_INT16_MAX or less than M4OSA_INT16_MIN
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrGetInt16(M4OSA_Char*        pStrIn,
                            M4OSA_Int16*    pVal,
                            M4OSA_Char**    pStrOut,
                            M4OSA_chrNumBase base)
{
    M4OSA_Int32 l;
    char*       pTemp;

    M4OSA_TRACE1_4("M4OSA_chrGetInt16\t(M4OSA_Char* %x, M4OSA_Int16* %x"
        "M4OSA_Char** %x,M4OSA_chrNumBase %d)",pStrIn,pVal,pStrOut,base);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn, M4ERR_PARAMETER,
                                    "M4OSA_chrGetInt16:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pVal, M4ERR_PARAMETER,
                                      "M4OSA_chrGetInt16:\tpVal is M4OSA_NULL");

    switch(base)
    {
    case M4OSA_kchrDec:
        l = strtol((const char *)pStrIn, &pTemp, 10);
        break;
    case M4OSA_kchrHexa:
        l = strtol((const char *)pStrIn, &pTemp, 16);
        break;
    case M4OSA_kchrOct:
        l = strtol((const char *)pStrIn, &pTemp, 8);
        break;
    default:
        return M4ERR_PARAMETER;
    }

    /* has conversion failed ? */
    if((M4OSA_Char*)pTemp == pStrIn)
    {
        *pVal = 0;
        return M4ERR_CHR_CONV_FAILED;
    }

    /* has an overflow occured ? */
    if(l>M4OSA_INT16_MAX)
    {
        *pVal = M4OSA_INT16_MAX;
        if(M4OSA_NULL != pStrOut)
        {
            *pStrOut = (M4OSA_Char*)pTemp;
        }
        return M4WAR_CHR_NUM_RANGE;
    }

    /* has an underflow occured ? */
    if(l<M4OSA_INT16_MIN)
    {
        *pVal = M4OSA_INT16_MIN;
        if(M4OSA_NULL != pStrOut)
        {
            *pStrOut = (M4OSA_Char*)pTemp;
        }
        return M4WAR_CHR_NUM_RANGE;
    }

    /* nominal case */
    *pVal = (M4OSA_UInt16)l;
    if(M4OSA_NULL != pStrOut)
    {
        *pStrOut = (M4OSA_Char*)pTemp;
    }

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function gets a M4OSA_Int8 from string.
 * @note       This function converts the first set of non-whitespace
 *             characters of pStrIn to a M4OSA_Int8 value pVal, assuming a
 *             representation in base provided by the parameter base. pStrOut is
 *             set to the first character of the string following the last
 *             character of the number that has been converted.
 *             - in case of a failure during the conversion, pStrOut is not
 *               updated, and pVal is set to null.
 *             - in case of numerical overflow or underflow, pVal is set to
 *               M4OSA_INT8_MAX or M4OSA_INT8_MIN respectively.
 *             - if pStrOut is not to be used, it can be set to M4OSA_NULL.
 * @param      pStrIn: (IN) Character string.
 * @param      pVal: (OUT) read value.
 * @param      pStrOut: (OUT) Output character string.
 * @param      base: (IN) Base of the character string representation.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pVal is M4OSA_NULL.
 * @return     M4ERR_CHR_CONV_FAILED: conversion failure.
 * @return     M4WAR_CHR_NUM_RANGE: the character string represents a number
 *             greater than M4OSA_INT8_MAX or less than M4OSA_INT8_MIN
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrGetInt8(M4OSA_Char*    pStrIn,
                           M4OSA_Int8*    pVal,
                           M4OSA_Char**    pStrOut,
                           M4OSA_chrNumBase base)
{
    M4OSA_Int32 l;
    char*       pTemp;

    M4OSA_TRACE1_4("M4OSA_chrGetInt8\t(M4OSA_Char* %x, M4OSA_Int8* %x"
                   "M4OSA_Char** %x,M4OSA_chrNumBase %d)",pStrIn,pVal,pStrOut,
                                                                          base);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn, M4ERR_PARAMETER,
                                     "M4OSA_chrGetInt8:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pVal, M4ERR_PARAMETER,
                                       "M4OSA_chrGetInt8:\tpVal is M4OSA_NULL");

    switch(base)
    {
    case M4OSA_kchrDec:
        l = strtol((const char *)pStrIn, &pTemp, 10);
        break;
    case M4OSA_kchrHexa:
        l = strtol((const char *)pStrIn, &pTemp, 16);
        break;
    case M4OSA_kchrOct:
        l = strtol((const char *)pStrIn, &pTemp, 8);
        break;
    default:
        return M4ERR_PARAMETER;
    }

    /* has conversion failed ? */
    if((M4OSA_Char*)pTemp == pStrIn)
    {
        *pVal = 0;
        return M4ERR_CHR_CONV_FAILED;
    }

    /* has an overflow occured ? */
    if(l>M4OSA_INT8_MAX)
    {
        *pVal = M4OSA_INT8_MAX;
        if(M4OSA_NULL != pStrOut)
        {
            *pStrOut = (M4OSA_Char*)pTemp;
        }
        return M4WAR_CHR_NUM_RANGE;
    }

    /* has an underflow occured ? */
    if(l<M4OSA_INT8_MIN)
    {
        *pVal = M4OSA_INT8_MIN;
        if(M4OSA_NULL != pStrOut)
        {
            *pStrOut = (M4OSA_Char*)pTemp;
        }
        return M4WAR_CHR_NUM_RANGE;
    }

    /* nominal case */
    *pVal = (M4OSA_UInt8)l;
    if(M4OSA_NULL != pStrOut)
    {
        *pStrOut = (M4OSA_Char*)pTemp;
    }

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function gets a M4OSA_Double from string.
 * @note       This function converts the first set of non-whitespace
 *             characters of pStrIn to a M4OSA_Double value pVal. pStrOut is set
 *             to the first character of the string following the last
 *             character of the number that has been converted.
 *             - in case of a failure during the conversion, pStrOut is not
 *               updated, and pVal is set to null.
 *             - in case of numerical overflow or underflow, pVal is set to null
 *             - if pStrOut is not to be used, it can be set to M4OSA_NULL.
 * @param      pStrIn: (IN) Character string.
 * @param      pVal: (OUT) read value.
 * @param      pStrOut: (OUT) Output character string.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pVal is M4OSA_NULL.
 * @return     M4ERR_CHR_CONV_FAILED: conversion failure.
 * @return     M4WAR_CHR_NUM_RANGE: an underflow or overflow occurs during the
 *             conversion.
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrGetDouble(M4OSA_Char*    pStrIn,
                             M4OSA_Double*    pVal,
                             M4OSA_Char**    pStrOut)
{
    M4OSA_Double d;
    char*        pTemp;

    M4OSA_TRACE1_3("M4OSA_chrGetDouble\t(M4OSA_Char* %x, M4OSA_Double* %x"
        "M4OSA_Char** %x)",pStrIn,pVal,pStrOut);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn, M4ERR_PARAMETER,
                                   "M4OSA_chrGetDouble:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pVal, M4ERR_PARAMETER,
                                     "M4OSA_chrGetDouble:\tpVal is M4OSA_NULL");

    errno = 0;
    d = strtod((const char *)pStrIn, &pTemp);

    /* has conversion failed ? */
    if((M4OSA_Char*)pTemp == pStrIn)
    {
        *pVal = 0.0;
        return M4ERR_CHR_CONV_FAILED;
    }

    /* has an overflow or underflow occured ? */
    if(errno == ERANGE)
    {
        *pVal = 0.0;
        if(M4OSA_NULL != pStrOut)
        {
            *pStrOut = (M4OSA_Char*)pTemp;
        }
        return M4WAR_CHR_NUM_RANGE;
    }

    /* nominal case */
    *pVal = (M4OSA_Double)d;
    if(M4OSA_NULL != pStrOut)
    {
        *pStrOut = (M4OSA_Char*)pTemp;
    }

    return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function gets a M4OSA_Time from string.
 * @note       Since, M4OSA_Time is defined as M4OSA_Int64, it calls
 *             M4OSA_chrGetInt64().
 * @param      pStrIn: (IN) Character string.
 * @param      pVal: (OUT) read value.
 * @param      pStrOut: (OUT) Output character string.
 * @param      base: (IN) Base of the character string representation.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pVal is M4OSA_NULL.
 * @return     M4ERR_CHR_CONV_FAILED: conversion failure.
 * @return     M4WAR_CHR_NUM_RANGE: the character string represents a number
 *             out of range.
 ************************************************************************
*/
M4OSA_ERR M4OSA_chrGetTime(M4OSA_Char*    pStrIn,
                           M4OSA_Time*    pVal,
                           M4OSA_Char**    pStrOut,
                           M4OSA_chrNumBase base)
{
    M4OSA_TRACE1_4("M4OSA_chrGetTime\t(M4OSA_Char* %x, M4OSA_Time* %x"
        "M4OSA_Char** %x,M4OSA_chrNumBase %d)",pStrIn,pVal,pStrOut,base);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn, M4ERR_PARAMETER,
                                     "M4OSA_chrGetTime:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pVal, M4ERR_PARAMETER,
                                       "M4OSA_chrGetTime:\tpVal is M4OSA_NULL");

    return M4OSA_chrGetInt64(pStrIn,(M4OSA_Int64*)pVal,pStrOut,base);
}

/**
 ************************************************************************
 * @brief      This function gets a M4OSA_FilePosition from string.
 * @note       Depending on the M4OSA_FilePosition definition, this function
 *             calls the correspoding underlying type.
 * @param      pStrIn: (IN) Character string.
 * @param      pVal: (OUT) read value.
 * @param      pStrOut: (OUT) Output character string.
 * @param      base: (IN) Base of the character string representation.
 * @return     M4NO_ERROR: there is no error.
 * @return     M4ERR_PARAMETER: pStrIn or pVal is M4OSA_NULL.
 * @return     M4ERR_CHR_CONV_FAILED: conversion failure.
 * @return     M4WAR_CHR_NUM_RANGE: the character string represents a number
 *             out of range.
 ******************************************************************************
*/
M4OSA_ERR M4OSA_chrGetFilePosition(M4OSA_Char*            pStrIn,
                                   M4OSA_FilePosition*    pVal,
                                   M4OSA_Char**            pStrOut,
                                   M4OSA_chrNumBase        base)
{
    M4OSA_TRACE1_4("M4OSA_chrGetFilePosition\t(M4OSA_Char* %x, M4OSA_FilePosition* %x"
        "M4OSA_Char** %x,M4OSA_chrNumBase %d)",pStrIn,pVal,pStrOut,base);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrIn, M4ERR_PARAMETER,
                             "M4OSA_chrGetFilePosition:\tpStrIn is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == pVal, M4ERR_PARAMETER,
                               "M4OSA_chrGetFilePosition:\tpVal is M4OSA_NULL");

#ifdef M4OSA_FILE_POS_64_BITS_SUPPORTED
    return M4OSA_chrGetInt64(pStrIn,(M4OSA_Int64*)pVal,pStrOut,base);
#else
    return M4OSA_chrGetInt32(pStrIn,(M4OSA_Int32*)pVal,pStrOut,base);
#endif

}

M4OSA_ERR M4OSA_chrSPrintf(M4OSA_Char  *pStrOut, M4OSA_UInt32 strOutMaxLen,
                           M4OSA_Char   *format, ...)
{
    va_list       marker;
    M4OSA_Char   *pTemp;
    M4OSA_Char   *percentPointer;
    M4OSA_Char   *newFormat;
    M4OSA_Int32  newFormatLength=0;
    M4OSA_UInt32  count_ll = 0;
    M4OSA_UInt32  count_tm = 0;
    M4OSA_UInt32  count_aa = 0;
    M4OSA_UInt32  count;
    M4OSA_UInt32  nbChar;
    M4OSA_Int32     err;
    M4OSA_Char flagChar[]             = "'-+ #0";
    M4OSA_Char widthOrPrecisionChar[] = "*0123456789";
    M4OSA_Char otherPrefixChar[]      = "hlL";
    M4OSA_Char conversionChar[]       = "diouxXnfeEgGcCsSp%";

    M4OSA_TRACE1_3("M4OSA_chrSPrintf\t(M4OSA_Char* %x, M4OSA_UInt32 %ld"
        "M4OSA_Char* %x)",pStrOut,strOutMaxLen,format);
    M4OSA_DEBUG_IF2(M4OSA_NULL == pStrOut, M4ERR_PARAMETER,
                                    "M4OSA_chrSPrintf:\tpStrOut is M4OSA_NULL");
    M4OSA_DEBUG_IF2(M4OSA_NULL == format, M4ERR_PARAMETER,
                                     "M4OSA_chrSPrintf:\tformat is M4OSA_NULL");

    va_start(marker,format);

    /* count the number of %[flags][width][.precision]ll[conversion] */
    pTemp = format;
    while(*pTemp)
    {
        percentPointer = (M4OSA_Char *)strchr((const char *)pTemp,'%'); /* get the next percent character */
        if(!percentPointer)
            break;                            /* "This is the End", (c) J. Morrisson */
        pTemp = percentPointer+1;           /* span it */
        if(!*pTemp)
            break;                            /* "This is the End", (c) J. Morrisson */
        pTemp += strspn((const char *)pTemp,(const char *)flagChar);    /* span the optional flags */
        if(!*pTemp)
            break;                            /* "This is the End", (c) J. Morrisson */
        pTemp += strspn((const char *)pTemp,(const char *)widthOrPrecisionChar); /* span the optional width */
        if(!*pTemp)
            break;                            /* "This is the End", (c) J. Morrisson */
        if(*pTemp=='.')
        {
            pTemp++;
            pTemp += strspn((const char *)pTemp, (const char *)widthOrPrecisionChar); /* span the optional precision */
        }
        if(!*pTemp)
            break;                            /* "This is the End", (c) J. Morrisson */
        if(strlen((const char *)pTemp)>=2)
        {
            if(!strncmp((const char *)pTemp,"ll",2))
            {
                count_ll++;                 /* I got ONE */
                pTemp +=2;                  /* span the "ll" prefix */
            }
            else if(!strncmp((const char *)pTemp,"tm",2))
            {
                count_tm++;
                pTemp +=2;
            }
            else if(!strncmp((const char *)pTemp,"aa",2))
            {
                count_aa++;
                pTemp +=2;
            }
        }
        pTemp += strspn((const char *)pTemp, (const char *)otherPrefixChar); /* span the other optional prefix */
        if(!*pTemp)
            break;                        /* "This is the End", (c) J. Morrisson */
        pTemp += strspn((const char *)pTemp, (const char *)conversionChar);
        if(!*pTemp)
            break;                        /* "This is the End", (c) J. Morrisson */
    }

    count = count_ll + count_tm + count_aa;

    if(!count)
    {
        err= vsnprintf((char *)pStrOut, (size_t)strOutMaxLen + 1, (const char *)format, marker);
        va_end(marker);
        if ((err<0) || ((M4OSA_UInt32)err>strOutMaxLen))
        {
            pStrOut[strOutMaxLen] = '\0';
            return M4ERR_CHR_STR_OVERFLOW;
        }
        else
        {
            return M4NO_ERROR;
        }
    }


    newFormatLength = strlen((const char *)format) + 1;

#ifdef M4OSA_64BITS_SUPPORTED
#ifdef M4OSA_FILE_POS_64_BITS_SUPPORTED
    newFormatLength += (count_ll+count_tm+count_aa);
#else
    newFormatLength += (count_ll+count_tm-count_aa);
#endif
#elif defined M4OSA_64BITS_NOT_SUPPORTED
    newFormatLength -= (count_ll+count_tm+count_aa);
#else
    return M4ERR_NOT_IMPLEMENTED;
#endif

    newFormat =(M4OSA_Char*)M4OSA_malloc(newFormatLength,
        M4OSA_CHARSTAR,(M4OSA_Char*)"M4OSA_chrPrintf: newFormat");
    if(M4OSA_NULL == newFormat)
        return M4ERR_ALLOC;
    newFormat[newFormatLength-1] = '\0';
    pTemp = newFormat;

    /* copy format to newFormat, replacing %[flags][width][.precision]ll[conversion]
     * by %[flags][width][.precision]I64[conversion] */
    while(*format)
    {
        nbChar = strcspn((const char *)format, "%");
        if(nbChar)
        {
            strncpy((char *)pTemp, (const char *)format, nbChar);      /* copy characters before the % character */
            format +=nbChar;
            pTemp   +=nbChar;
        }
        if(!*format) break;
        *pTemp++ = *format++;                 /* copy the % character */
        nbChar = strspn((const char *)format, (const char *)flagChar);
        if(nbChar)
        {
            strncpy((char *)pTemp, (const char *)format, nbChar);      /* copy the flag characters */
            format +=nbChar;
            pTemp   +=nbChar;
        }
        if(!*format) break;
        nbChar = strspn((const char *)format, (const char *)widthOrPrecisionChar);
        if(nbChar)
        {
            strncpy((char *)pTemp, (const char *)format, nbChar);      /* copy the width characters */
            format +=nbChar;
            pTemp   +=nbChar;
        }
        if(!*format) break;
        if(*format=='.')
        {
            *pTemp++ = *format++;              /* copy the dot character */
            if(!format) break;
            nbChar = strspn((const char *)format, (const char *)widthOrPrecisionChar);
            if(nbChar)
            {
                strncpy((char *)pTemp, (const char *)format, nbChar);      /* copy the width characters */
                format +=nbChar;
                pTemp   +=nbChar;
            }
            if(!format) break;
        }
        if(strlen((const char *)format)>=2)
        {
            if(!strncmp((const char *)format, "ll", 2))
            {
#ifdef M4OSA_64BITS_SUPPORTED
                *pTemp++ = 'l'; /* %ll */
                *pTemp++ = 'l';
#else
                *pTemp++ = 'l'; /* %l */
#endif
                format +=2;                         /* span the "ll" prefix */
            }
            else if(!strncmp((const char *)format, "tm", 2))
            {
#ifdef M4OSA_64BITS_SUPPORTED
                *pTemp++ = 'l'; /* %ll */
                *pTemp++ = 'l';
#else
                *pTemp++ = 'l'; /* %l */
#endif
                format +=2;                         /* span the "tm" prefix */
            }
            else if(!strncmp((const char *)format, "aa", 2))
            {
#ifdef M4OSA_64BITS_SUPPORTED
#ifdef M4OSA_FILE_POS_64_BITS_SUPPORTED
                *pTemp++ = 'l'; /* %ll */
                *pTemp++ = 'l';
#else
                *pTemp++ = 'l';
#endif
#else
                *pTemp++ = 'l';
#endif
                format +=2;                         /* span the "aa" prefix */
            }
        }
        nbChar = strspn((const char *)format, (const char *)otherPrefixChar);
        if(nbChar)
        {
            strncpy((char *)pTemp, (const char *)format, nbChar);      /* copy the other Prefix */
            format +=nbChar;
            pTemp   +=nbChar;
        }
        if(!*format) break;
        nbChar = strspn((const char *)format, (const char *)conversionChar);
        if(nbChar)
        {
            strncpy((char *)pTemp, (const char *)format, nbChar);
            format += nbChar;
            pTemp   += nbChar;
        }
        if(!*format) break;
    }

    /* Zero terminate the format string. */
    (*pTemp) = '\0';

    err = vsnprintf((char *)pStrOut, (size_t)strOutMaxLen + 1, (const char *)newFormat, marker);
    va_end(marker);
    M4OSA_free((M4OSA_MemAddr32)newFormat);
    if ((err<0) || ((M4OSA_UInt32)err>strOutMaxLen))
    {
        pStrOut[strOutMaxLen] = '\0';
        return M4ERR_CHR_STR_OVERFLOW;
    }
    else
    {
        return M4NO_ERROR;
    }
}

