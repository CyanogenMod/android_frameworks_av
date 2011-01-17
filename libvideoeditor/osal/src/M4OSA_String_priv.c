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
 * @file         M4OSA_String_priv.c
 ************************************************************************
*/

#include "M4OSA_Debug.h"
#include "M4OSA_Types.h"
#include "M4OSA_Memory.h"
#include "M4OSA_Error.h"
#include "M4OSA_String_priv.h"
#include "M4OSA_String.h"

/**
 ************************************************************************
 * @brief      This function replaces a string buffer by a new "C-String"
 *             and manage memory if needed
 * @note
 * @param      pstr_src
 * @param      pac_in
 * @return     M4OSA_ERROR
 ************************************************************************
 */
M4OSA_ERR M4OSA_strPrivRealloc(M4OSA_strStruct* str,
                               M4OSA_UInt32 ui32_length)
{
   M4OSA_UInt32 ui32_size;
   M4OSA_Char* buffer;

   M4OSA_TRACE2_2("M4OSA_strPrivRealloc\t\tM4OSA_strStruct* 0x%x\t"
                  "M4OSA_UInt32* %d", str, ui32_length);

   ui32_size = str->ui32_size;

   /* Realloc if size is not sufficient to contain it entirely */
   if(ui32_length >= ui32_size)
   {
      if(ui32_size == 0)
      {
         ui32_size = 16;
      }

      while(ui32_length >= ui32_size)
      {
         ui32_size <<= 1;
      }

      buffer = str->pui8_buffer;

      if(buffer != M4OSA_NULL)
      {
         M4OSA_free((M4OSA_MemAddr32)buffer);
      }

      /* Allocate the actual M4OSA_String content */
      buffer = (M4OSA_Char*)M4OSA_malloc(ui32_size * sizeof(M4OSA_Char),
                            M4OSA_STRING, (M4OSA_Char*)"M4OSA_strPrivRealloc");

      /* Check memory allocation error */
      if(buffer == M4OSA_NULL)
      {
         M4OSA_DEBUG(M4ERR_ALLOC, "M4OSA_strPrivRealloc");

         str->pui8_buffer = M4OSA_NULL;
         str->ui32_size = 0;
         str->ui32_length = 0;

         return M4ERR_ALLOC;
      }

      str->pui8_buffer = buffer;
      str->ui32_size = ui32_size;
   }

   return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function replaces a string buffer by a new "C-String"
 *             and manage memory if needed
 * @note
 * @param      pstr_src
 * @param      pac_in
 * @return     M4OSA_ERROR
 ************************************************************************
 */
M4OSA_ERR M4OSA_strPrivReallocCopy(M4OSA_strStruct* str,
                                   M4OSA_UInt32 ui32_length)
{
   M4OSA_UInt32 ui32_size;
   M4OSA_Char* buffer;
   M4OSA_Char* pui8_buffer;

   M4OSA_TRACE2_2("M4OSA_strPrivReallocCopy\t\tM4OSA_strStruct* 0x%x\t"
                  "M4OSA_UInt32* %d", str, ui32_length);


   ui32_size = str->ui32_size;

   /* Realloc if size is not sufficient to contain it entirely */
   if(ui32_length >= ui32_size)
   {
      if(ui32_size == 0)
      {
         ui32_size = 16;
      }

      while(ui32_length >= ui32_size)
      {
         ui32_size <<= 1;
      }

      /* Allocate the actual M4OSA_String content */
      buffer = (M4OSA_Char*)M4OSA_malloc(ui32_size * sizeof(M4OSA_Char),
                            M4OSA_STRING, (M4OSA_Char*)"M4OSA_strPrivReallocCopy");

      /* Check memory allocation error */
      if(buffer == M4OSA_NULL)
      {
         M4OSA_DEBUG(M4ERR_ALLOC, "M4OSA_strPrivReallocCopy");

         str->pui8_buffer = M4OSA_NULL;
         str->ui32_size = 0;
         str->ui32_length = 0;

         return M4ERR_ALLOC;
      }

      pui8_buffer = str->pui8_buffer;

      if(pui8_buffer != M4OSA_NULL)
      {
         M4OSA_memcpy(buffer, pui8_buffer, str->ui32_length + 1);

         M4OSA_free((M4OSA_MemAddr32)pui8_buffer);
      }

      str->pui8_buffer = buffer;
      str->ui32_size = ui32_size;
   }

   return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strPrivDuplicate(M4OSA_strStruct** ostr,
                                 M4OSA_strStruct* istr)
{
   M4OSA_strStruct* str;
   M4OSA_ERR err_code;

   M4OSA_TRACE2_2("M4OSA_strPrivDuplicate\t\tM4OSA_strStruct** 0x%x\t"
                  "M4OSA_strStruct** 0x%x", ostr, istr);

   /* Allocate the output M4OSA_String */
   str = (M4OSA_strStruct*)M4OSA_malloc(sizeof(M4OSA_strStruct), M4OSA_STRING,
                           (M4OSA_Char*)"M4OSA_strPrivDuplicate: output string");

   /* Check memory allocation error */
   if(str == M4OSA_NULL)
   {
      *ostr = M4OSA_NULL ;

      M4OSA_DEBUG(M4ERR_ALLOC, "M4OSA_strPrivDuplicate");

      return M4ERR_ALLOC;
   }

   str->coreID = M4OSA_STRING;
   str->pui8_buffer = M4OSA_NULL;
   str->ui32_length = 0;
   str->ui32_size = 0;

   err_code =  M4OSA_strPrivSet(str, istr->pui8_buffer, istr->ui32_length);

   if(err_code != M4NO_ERROR)
   {
      return err_code;
   }

   *ostr = str;

   return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function replaces a string buffer by a new "C-String"
 *             and manage memory if needed
 * @note
 * @param      pstr_src
 * @param      pac_in
 * @return     M4OSA_ERROR
 ************************************************************************
*/
M4OSA_ERR M4OSA_strPrivSet(M4OSA_strStruct* str,
                           M4OSA_Char* pchar,
                           M4OSA_UInt32 ui32_length)
{
   M4OSA_UInt32 length;
   M4OSA_Char* buffer;

   M4OSA_TRACE2_3("M4OSA_strPrivSet\t\tM4OSA_strStruct* 0x%x\tM4OSA_Char* "
                  "0x%x\tM4OSA_UInt32 %d", str, pchar, ui32_length);

   if(ui32_length != 0)
   {
      length = M4OSA_chrLength(pchar);

      if(length < ui32_length)
      {
         ui32_length = length;
      }

      if(M4OSA_strPrivRealloc(str, ui32_length) != M4NO_ERROR)
      {
         M4OSA_DEBUG(M4ERR_ALLOC, "M4OSA_strPrivSet");

         return M4ERR_ALLOC;
      }

      buffer = str->pui8_buffer;

      /* Fill the actual M4OSA_String content */
      M4OSA_memcpy(buffer, pchar, ui32_length);

      buffer[ui32_length] = '\0';
   }
   else if(str->pui8_buffer != M4OSA_NULL)
   {
      str->pui8_buffer[0] = '\0';
   }

   str->ui32_length = ui32_length;

   return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function replaces a string buffer by a new "C-String"
 *             and manage memory if needed
 * @note
 * @param      pstr_src
 * @param      pac_in
 * @return     M4OSA_ERROR
 ************************************************************************
*/
M4OSA_Int32 M4OSA_strPrivFindLastSubStr(M4OSA_strStruct* str1,
                                        M4OSA_strStruct* str2,
                                        M4OSA_UInt32 ui32_pos)
{
   M4OSA_Char *pchar;
   M4OSA_Char *buffer1;
   M4OSA_Char *buffer2;
   M4OSA_Int32 i32_result;
   M4OSA_UInt32 length1, length2;
   M4OSA_Int32 dist;

   M4OSA_TRACE2_3("M4OSA_strPrivFindLastSubStr\t\tM4OSA_strStruct* 0x%x\t"
                  "M4OSA_strStruct* 0x%x\tM4OSA_UInt32 %d",
                  str1, str2, ui32_pos);

   length1 = str1->ui32_length;
   length2 = str2->ui32_length;

   if((length1 == 0) || (length2 == 0))
   {
      return -1;
   }

   buffer1 = str1->pui8_buffer;
   buffer2 = str2->pui8_buffer;

   dist = length1 - length2;

   if(dist < 0)
   {
      return -1;
   }

   if((M4OSA_Int32)ui32_pos > dist)
   {
      ui32_pos = dist;
   }

   for(pchar = buffer1 + ui32_pos; pchar != buffer1; pchar--)
   {
      M4OSA_chrNCompare(pchar, buffer2, length2, &i32_result);

      if(i32_result == 0)
      {
         return pchar - buffer1;
      }
   }

   return -1;
}

/**
 ************************************************************************
 * @brief      This function replaces a string buffer by a new "C-String"
 *             and manage memory if needed
 * @note
 * @param      pstr_src
 * @param      pac_in
 * @return     M4OSA_ERROR
 ************************************************************************
 */
M4OSA_ERR M4OSA_strPrivSetAndRepleceStr(M4OSA_strStruct* istr,
                                        M4OSA_UInt32 ui32_pos,
                                        M4OSA_UInt32 olength,
                                        M4OSA_Char* nbuff,
                                        M4OSA_UInt32 nlength)
{
   M4OSA_Char* buffer;
   M4OSA_Char* ibuffer;
   M4OSA_UInt32 ui32_length, ui32_size, ui32_lend, ui32_poso, ui32_posn;

   M4OSA_TRACE2_5("M4OSA_strPrivSetAndRepleceStr\t\tM4OSA_strStruct* 0x%x\t"
                  "M4OSA_UInt32 %d\tM4OSA_UInt32 %d\tM4OSA_Char* 0x%x\t"
                  "M4OSA_UInt32 %d", istr, ui32_pos, olength, nbuff, nlength);

   ui32_length = istr->ui32_length - olength + nlength;

   ibuffer = istr->pui8_buffer;

   /* string to replace has the same this that new string */
   if(nlength == olength)
   {
      if(nlength == 0)
      {
         return M4NO_ERROR;
      }

      M4OSA_memcpy(ibuffer + ui32_pos, nbuff, nlength);
   }
   else
   {
      ui32_lend = istr->ui32_length - ui32_pos - olength;
      ui32_poso = ui32_pos + olength;
      ui32_posn = ui32_pos + nlength;

      /* string to replace is bigger that new string */
      if(nlength < olength)
      {
         if(nlength > 0)
         {
            M4OSA_memcpy(ibuffer + ui32_pos, nbuff, nlength);
         }

         if((olength - nlength) >= ui32_lend)
         {
            M4OSA_memcpy(ibuffer + ui32_posn, ibuffer + ui32_poso, ui32_lend);
         }
         else
         {
            buffer = (M4OSA_Char*)M4OSA_malloc(ui32_lend * sizeof(M4OSA_Char),
                                M4OSA_STRING, (M4OSA_Char*)"M4OSA_strPrivSetAndRepleceStr");

            M4OSA_CHECK_MALLOC(buffer, "M4OSA_strPrivSetAndRepleceStr");
            M4OSA_memcpy(buffer, ibuffer + ui32_poso, ui32_lend);
            M4OSA_memcpy(ibuffer + ui32_posn, buffer, ui32_lend);
            M4OSA_free((M4OSA_MemAddr32)buffer);
         }
      }
      /* string to replace is smaller that new string */
      else
      {
         ui32_size = istr->ui32_size;

         /* check if there is enough memory allocated in istr */
         if(ui32_length >= ui32_size)
         {
            if(ui32_size == 0)
            {
               ui32_size = 16;
            }

            while(ui32_length >= ui32_size)
            {
               ui32_size <<= 1;
            }

            buffer = (M4OSA_Char*)M4OSA_malloc(ui32_size * sizeof(M4OSA_Char),
                                M4OSA_STRING, (M4OSA_Char*)"M4OSA_strPrivSetAndRepleceStr");

            M4OSA_CHECK_MALLOC(buffer, "M4OSA_strPrivSetAndRepleceStr");

            M4OSA_memcpy(buffer, ibuffer, ui32_pos);

            M4OSA_memcpy(buffer + ui32_pos, nbuff, nlength);

            M4OSA_memcpy(buffer + ui32_posn, ibuffer + ui32_poso, ui32_lend);

            M4OSA_free((M4OSA_MemAddr32)ibuffer);

            istr->pui8_buffer = buffer;

            istr->ui32_size = ui32_size;
         }
         else
         {
            buffer = (M4OSA_Char*)M4OSA_malloc(ui32_lend * sizeof(M4OSA_Char),
                                M4OSA_STRING, (M4OSA_Char*)"M4OSA_strPrivSetAndRepleceStr");

            M4OSA_CHECK_MALLOC(buffer, "M4OSA_strPrivSetAndRepleceStr");

            M4OSA_memcpy(buffer, ibuffer + ui32_poso, ui32_lend);

            M4OSA_memcpy(ibuffer + ui32_pos, nbuff, nlength);

            M4OSA_memcpy(ibuffer + ui32_posn, buffer, ui32_lend);

            M4OSA_free((M4OSA_MemAddr32)buffer);
         }
      }
   }


   istr->pui8_buffer[ui32_length] = '\0';

   istr->ui32_length = ui32_length;

   return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function replaces a string buffer by a new "C-String"
 *             and manage memory if needed
 * @note
 * @param      pstr_src
 * @param      pac_in
 * @return     M4OSA_ERROR
 ************************************************************************
 */
M4OSA_ERR M4OSA_strPrivReplaceSameSizeStr(M4OSA_strStruct* istr,
                                          M4OSA_strStruct* ostr,
                                          M4OSA_strStruct* nstr,
                                          M4OSA_strMode mode)
{
   M4OSA_Char* ibuffer;
   M4OSA_Char* obuffer;
   M4OSA_Char* nbuffer;
   M4OSA_Char* ptr;
   M4OSA_UInt32 ilength, nlength;
   M4OSA_Int32 i32_pos;
   M4OSA_ERR err_code;

   M4OSA_TRACE2_4("M4OSA_strPrivReplaceSameSizeStr\t\tM4OSA_strStruct* 0x%x\t"
                  "M4OSA_strStruct* 0x%x\tM4OSA_strStruct* 0x%x\t"
                  "M4OSA_strMode %d", istr, ostr, nstr, mode);

   ibuffer = istr->pui8_buffer;
   obuffer = ostr->pui8_buffer;
   nbuffer = nstr->pui8_buffer;

   ilength = istr->ui32_length;
   nlength = nstr->ui32_length;

   if(mode != M4OSA_kstrEnd)
   {
      err_code = M4OSA_chrFindPattern(ibuffer, obuffer, &ptr);

      M4OSA_DEBUG_IF2(M4OSA_ERR_IS_ERROR(err_code), err_code,
                      "M4OSA_strPrivReplaceSameSizeStr");

      if(err_code == M4WAR_CHR_NOT_FOUND)
      {
         return M4WAR_STR_NOT_FOUND;
      }

      if(mode == M4OSA_kstrAll)
      {
         do
         {
            M4OSA_memcpy(ptr, nbuffer, nlength);

            err_code = M4OSA_chrFindPattern(ptr+nlength, obuffer, &ptr);

            M4OSA_DEBUG_IF2(M4OSA_ERR_IS_ERROR(err_code), err_code,
                            "M4OSA_strPrivReplaceSameSizeStr");

         } while(err_code != M4WAR_CHR_NOT_FOUND);
      }
      else
      {
         M4OSA_memcpy(ptr, nbuffer, nlength);
      }
   }
   else
   {
      i32_pos = M4OSA_strPrivFindLastSubStr(istr, ostr, ilength-1);

      if(i32_pos < 0)
      {
         return M4WAR_STR_NOT_FOUND;
      }

      M4OSA_memcpy(ibuffer + i32_pos, nbuffer, nlength);
   }


   return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function replaces a string buffer by a new "C-String"
 *             and manage memory if needed
 * @note
 * @param      pstr_src
 * @param      pac_in
 * @return     M4OSA_ERROR
 ************************************************************************
 */
M4OSA_ERR M4OSA_strPrivReplaceSmallerStr(M4OSA_strStruct* istr,
                                         M4OSA_strStruct* ostr,
                                         M4OSA_strStruct* nstr,
                                         M4OSA_strMode mode)
{
   M4OSA_Char* ibuffer;
   M4OSA_Char* obuffer;
   M4OSA_Char* nbuffer;
   M4OSA_Char* buffer;
   M4OSA_Char* ptr_src;
   M4OSA_Char* ptr_dest;
   M4OSA_UInt32 ilength, nlength, olength, size, length;
   M4OSA_Int32 i32_pos;
   M4OSA_ERR err_code;

   M4OSA_TRACE2_4("M4OSA_strPrivReplaceSmallerStr\t\tM4OSA_strStruct* 0x%x\t"
                  "M4OSA_strStruct* 0x%x\tM4OSA_strStruct* 0x%x\t"
                  "M4OSA_strMode %d", istr, ostr, nstr, mode);

   ibuffer = istr->pui8_buffer;
   obuffer = ostr->pui8_buffer;
   nbuffer = nstr->pui8_buffer;

   ilength = istr->ui32_length;
   olength = ostr->ui32_length;
   nlength = nstr->ui32_length;

   length = ilength;

   if(mode == M4OSA_kstrAll)
   {
      err_code = M4OSA_chrFindPattern(ibuffer, obuffer, &ptr_src);

      M4OSA_DEBUG_IF2(M4OSA_ERR_IS_ERROR(err_code), err_code,
                      "M4OSA_strPrivReplaceSameSizeStr");

      if(err_code == M4WAR_CHR_NOT_FOUND)
      {
         return M4WAR_STR_NOT_FOUND;
      }

      /* Allocate the actual M4OSA_String content */
      buffer = (M4OSA_Char*)M4OSA_malloc(istr->ui32_size * sizeof(M4OSA_Char),
                            M4OSA_STRING, (M4OSA_Char*)"M4OSA_strPrivReplaceSmallerStr");

      M4OSA_CHECK_MALLOC(buffer, "M4OSA_strPrivReplaceSmallerStr");

      ptr_dest = buffer;

      do
      {
         size = (M4OSA_UInt32)ptr_src - (M4OSA_UInt32)ibuffer;

         length += (nlength - olength);

         M4OSA_memcpy(ptr_dest, ibuffer, size);

         ptr_dest += size;

         M4OSA_memcpy(ptr_dest, nbuffer, nlength);

         ptr_dest += nlength;

         ibuffer += (size + olength);

         err_code = M4OSA_chrFindPattern(ibuffer, obuffer, &ptr_src);

         M4OSA_DEBUG_IF2(M4OSA_ERR_IS_ERROR(err_code), err_code,
                         "M4OSA_strPrivReplaceSameSizeStr");

      } while(err_code != M4WAR_CHR_NOT_FOUND);

      size = ilength - (M4OSA_UInt32)(ibuffer - istr->pui8_buffer);

      M4OSA_memcpy(ptr_dest, ibuffer, size);

      M4OSA_free((M4OSA_MemAddr32)istr->pui8_buffer);

      istr->ui32_length = length ;

      buffer[length] = '\0';

      istr->pui8_buffer = buffer;
   }
   else
   {
      if(mode == M4OSA_kstrBegin)
      {
         err_code = M4OSA_chrFindPattern(ibuffer, obuffer, &ptr_src);

         M4OSA_DEBUG_IF2(M4OSA_ERR_IS_ERROR(err_code), err_code,
                      "M4OSA_strPrivReplaceSameSizeStr");

         if(err_code == M4WAR_CHR_NOT_FOUND)
         {
            return M4WAR_STR_NOT_FOUND;
         }

         i32_pos = (M4OSA_UInt32)ptr_src - (M4OSA_UInt32)ibuffer;
      }
      else
      {
         i32_pos = M4OSA_strPrivFindLastSubStr(istr, ostr, ilength-1);

         if(i32_pos == -1)
         {
            return M4WAR_STR_NOT_FOUND;
         }
      }

      err_code = M4OSA_strPrivSetAndRepleceStr(istr, i32_pos, olength,
                                               nbuffer, nlength);

      if(M4OSA_ERR_IS_ERROR(err_code))
      {
         M4OSA_DEBUG(err_code, "M4OSA_strPrivReplaceSmallerStr");

         return err_code;
      }
   }

   return M4NO_ERROR;
}

/**
 ************************************************************************
 * @brief      This function replaces a string buffer by a new "C-String"
 *             and manage memory if needed
 * @note
 * @param      pstr_src
 * @param      pac_in
 * @return     M4OSA_ERROR
 ************************************************************************
 */
M4OSA_ERR M4OSA_strPrivReplaceBiggerStr(M4OSA_strStruct* istr,
                                        M4OSA_strStruct* ostr,
                                        M4OSA_strStruct* nstr,
                                        M4OSA_strMode mode)
{
   M4OSA_Char* ibuffer;
   M4OSA_Char* obuffer;
   M4OSA_Char* nbuffer;
   M4OSA_Char* buffer;
   M4OSA_Char* ptr;
   M4OSA_UInt32 ilength, nlength, olength, length;
   M4OSA_Int32 i32_pos;
   M4OSA_ERR err_code;

   M4OSA_TRACE2_4("M4OSA_strPrivReplaceBiggerStr\t\tM4OSA_strStruct* 0x%x\t"
                  "M4OSA_strStruct* 0x%x\tM4OSA_strStruct* 0x%x\t"
                  "M4OSA_strMode %d", istr, ostr, nstr, mode);

   ibuffer = istr->pui8_buffer;
   obuffer = ostr->pui8_buffer;
   nbuffer = nstr->pui8_buffer;

   ilength = istr->ui32_length;
   olength = ostr->ui32_length;
   nlength = nstr->ui32_length;


   if(mode == M4OSA_kstrAll)
   {
      M4OSA_UInt32 n=0, i;
      M4OSA_Int32* patterns;
      M4OSA_UInt32 pos=0, size;
      M4OSA_UInt32 max_pattern = ilength / olength;
      M4OSA_UInt32 ui32_size = istr->ui32_size;
      M4OSA_Char* src;
      M4OSA_Char* dest;

      /* Allocate the actual M4OSA_String content */
      patterns = (M4OSA_Int32*)M4OSA_malloc(max_pattern * sizeof(M4OSA_UInt32),
                              M4OSA_STRING, (M4OSA_Char*)"M4OSA_strPrivReplaceBiggerStr");

      M4OSA_CHECK_MALLOC(patterns, (M4OSA_Char*)"M4OSA_strPrivReplaceBiggerStr");


      err_code = M4OSA_chrFindPattern(ibuffer, obuffer, &ptr);

      M4OSA_DEBUG_IF2(M4OSA_ERR_IS_ERROR(err_code), err_code,
                      "M4OSA_strPrivReplaceBiggerStr");

      if(err_code == M4WAR_CHR_NOT_FOUND)
      {
         return M4WAR_STR_NOT_FOUND;
      }

      do
      {
         patterns[n] = (M4OSA_UInt32)ptr - (M4OSA_UInt32)ibuffer;

         n++;

         err_code = M4OSA_chrFindPattern(ptr + olength, obuffer, &ptr);

         M4OSA_DEBUG_IF2(M4OSA_ERR_IS_ERROR(err_code), err_code,
                         "M4OSA_strPrivReplaceBiggerStr");

      } while(err_code != M4WAR_CHR_NOT_FOUND);

      length = ilength - (n * olength) + (n * nlength);


      if(length >= ui32_size)
      {
         do
         {
            ui32_size <<= 1;

         } while(length >= ui32_size);
      }

      /* Allocate the actual M4OSA_String content */
      buffer = (M4OSA_Char*)M4OSA_malloc(ui32_size * sizeof(M4OSA_Char),
                            M4OSA_STRING, (M4OSA_Char*)"M4OSA_strPrivReplaceBiggerStr");

      M4OSA_CHECK_MALLOC(buffer, "M4OSA_strPrivReplaceBiggerStr");

      src = ibuffer;
      dest = buffer;

      for(i=0; i<n; i++)
      {
         size = patterns[i] - pos;

         M4OSA_memcpy(dest, src, size);

         pos = patterns[i] + olength;

         src = ibuffer + pos;

         dest += size;

         M4OSA_memcpy(dest, nbuffer, nlength);

         dest += nlength;
      }

      size = ilength - (M4OSA_UInt32)(src - ibuffer);

      M4OSA_memcpy(dest, src, size);

      M4OSA_free((M4OSA_MemAddr32)patterns);

      M4OSA_free((M4OSA_MemAddr32)ibuffer);

      istr->ui32_length = length;

      istr->pui8_buffer = buffer;

      istr->pui8_buffer[length] = '\0';
   }
   else
   {
      if(mode == M4OSA_kstrBegin)
      {
         err_code = M4OSA_chrFindPattern(ibuffer, obuffer, &ptr);

         M4OSA_DEBUG_IF2(M4OSA_ERR_IS_ERROR(err_code), err_code,
                      "M4OSA_strPrivReplaceSameSizeStr");

         if(err_code == M4WAR_CHR_NOT_FOUND)
         {
            return M4WAR_STR_NOT_FOUND;
         }

         i32_pos = (M4OSA_UInt32)ptr - (M4OSA_UInt32)ibuffer;
      }
      else
      {
         i32_pos = M4OSA_strPrivFindLastSubStr(istr, ostr, ilength-1);

         if(i32_pos == -1)
         {
            return M4WAR_STR_NOT_FOUND;
         }
      }

      err_code = M4OSA_strPrivSetAndRepleceStr(istr, i32_pos, olength,
                                               nbuffer, nlength);

      if(M4OSA_ERR_IS_ERROR(err_code))
      {
         M4OSA_DEBUG(err_code, "M4OSA_strPrivReplaceSmallerStr");

         return err_code;
      }
   }

   return M4NO_ERROR;
}


M4OSA_ERR M4OSA_strPrivSPrintf(M4OSA_strStruct* str,
                               M4OSA_Char *format,
                               va_list marker)
{
    M4OSA_Char *temp;
    M4OSA_Char *percentPointer;
    M4OSA_Char *newFormat;
    M4OSA_Char* strOut = str->pui8_buffer + str->ui32_length;
    M4OSA_UInt32 strOutMaxLen = str->ui32_size-1;
    M4OSA_Int32 newFormatLength = 0;
    M4OSA_UInt32 count_ll = 0;
    M4OSA_UInt32 count_tm = 0;
    M4OSA_UInt32 count_aa = 0;
    M4OSA_UInt32 count;
    M4OSA_UInt32 nbChar;
    M4OSA_Int32  iResult;

    M4OSA_Int32 err;
    M4OSA_Char flagChar[]             = "'-+ #0";
    M4OSA_Char widthOrPrecisionChar[] = "*0123456789";
    M4OSA_Char otherPrefixChar[]      = "hlL";
    M4OSA_Char conversionChar[]       = "diouxXnfeEgGcCsSp%";

    M4OSA_TRACE2_2("M4OSA_strSPrintf\t\tM4OSA_String 0x%x\tM4OSA_Char* 0x%x",
        str, format);


    /* count the number of %[flags][width][.precision]ll[conversion] */
    temp = format;

    while(*temp)
    {
        /* get the next percent character */
        err = M4OSA_chrFindChar (temp, '%', &percentPointer);

        if((!percentPointer) || (M4WAR_CHR_NOT_FOUND == err))
        {
            break;         /* "This is the End", (c) J. Morrisson */
        }

        temp = percentPointer+1;           /* span it */
        if(!*temp) /* "This is the End", (c) J. Morrisson */
        {
            break;
        }

        /* span the optional flags */
        M4OSA_chrSpan(format, conversionChar, &nbChar);
        temp += nbChar;

        if(!*temp) /* "This is the End", (c) J. Morrisson */
        {
            break;
        }

        /* span the optional width */
        M4OSA_chrSpan(temp, widthOrPrecisionChar, &nbChar);
        temp += nbChar;
        if(!*temp) /* "This is the End", (c) J. Morrisson */
        {
            break;
        }

        if(*temp=='.')
        {
            /* span the optional precision */
            M4OSA_chrSpan(++temp, widthOrPrecisionChar, &nbChar);
            temp += nbChar;
        }
        if(!*temp) /* "This is the End", (c) J. Morrisson */
        {
            break;
        }

        if(M4OSA_chrLength(temp)>=2)
        {

            M4OSA_chrNCompare(temp, (M4OSA_Char*)"ll",2, &iResult);
            if (iResult != 0)
            {
                count_ll++;                        /* I got ONE */
                temp +=2;                          /* span the "ll" prefix */
            }
            else
            {
                M4OSA_chrNCompare(temp, (M4OSA_Char*)"tm",2, &iResult);
                if (iResult != 0) /* à voir si ce n'est pas == 0 */
                {
                    count_tm++;
                    temp +=2;
                }
                else
                {
                    M4OSA_chrNCompare(temp, (M4OSA_Char*)"aa",2, &iResult);
                    if (iResult != 0) /* à voir si ce n'est pas == 0 */
                    {
                        count_aa++;
                        temp +=2;
                    }
                }
            }
        }

        /* span the other optional prefix */
        M4OSA_chrSpan(temp, otherPrefixChar, &nbChar);
        temp += nbChar;
        if(!*temp) /* "This is the End", (c) J. Morrisson */
        {
            break;
        }

        M4OSA_chrSpan(temp, conversionChar, &nbChar);
        temp += nbChar;
        if(!*temp) /* "This is the End", (c) J. Morrisson */
        {
            break;
        }

    }

    count = count_ll + count_tm + count_aa;

    if(!count)
    {
        err = M4OSA_chrSPrintf(strOut,strOutMaxLen,format,marker);

        if(M4ERR_CHR_STR_OVERFLOW == err)
        {
            return M4ERR_STR_OVERFLOW;
        }

        return M4NO_ERROR;
    }


    newFormatLength = M4OSA_chrLength(format) + 1;

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
        M4OSA_CHARSTAR, (M4OSA_Char*)"M4OSA_chrPrintf: newFormat");
    if(newFormat == M4OSA_NULL) return M4ERR_ALLOC;
    newFormat[newFormatLength-1] = '\0';
    temp = newFormat;
    /* copy format to newFormat, replacing
    %[flags][width][.precision]ll[conversion]
    by %[flags][width][.precision]I64[conversion] */
    while(*format)
    {
        M4OSA_chrSpanComplement(format, (M4OSA_Char*)"%", &nbChar);
        if(nbChar)
        {
            M4OSA_chrNCopy(temp,format,nbChar);      /* copy characters before the % character */
            format +=nbChar;
            temp   +=nbChar;
        }
        if(!*format)
        {
            break;
        }
        *temp++ = *format++;                 /* copy the % character */

        M4OSA_chrSpan(format, flagChar, &nbChar);
        if(nbChar)
        {
            M4OSA_chrNCopy(temp,format,nbChar);      /* copy the flag characters */
            format +=nbChar;
            temp   +=nbChar;
        }
        if(!*format)
        {
            break;
        }

        M4OSA_chrSpan(format, widthOrPrecisionChar, &nbChar);
        if(nbChar)
        {
            M4OSA_chrNCopy(temp,format,nbChar);      /* copy the width characters */
            format +=nbChar;
            temp   +=nbChar;
        }
        if(!*format)
        {
            break;
        }
        if(*format=='.')
        {
            *temp++ = *format++;              /* copy the dot character */
            if(!*format)
            {
                break;
            }

            M4OSA_chrSpan(format, widthOrPrecisionChar, &nbChar);
            if(nbChar)
            {
                M4OSA_chrNCopy(temp,format,nbChar);      /* copy the width characters */
                format +=nbChar;
                temp   +=nbChar;
            }
            if(!*format)
            {
                break;
            }
        }
        if(M4OSA_chrLength(format)>=2)
        {

            M4OSA_chrNCompare(format, (M4OSA_Char*)"ll",2, &iResult);
            if (iResult != 0)
            {
#ifdef M4OSA_64BITS_SUPPORTED
                *temp++ = 'I'; /* %I64 */
                *temp++ = '6';
                *temp++ = '4';
#else
                *temp++ = 'l'; /* %l */
#endif
                format +=2;                         /* span the "ll" prefix */
            }
            else
            {
                M4OSA_chrNCompare(format, (M4OSA_Char*)"tm",2, &iResult);
                if (iResult != 0)
                {
#ifdef M4OSA_64BITS_SUPPORTED
                *temp++ = 'I'; /* %I64 */
                *temp++ = '6';
                *temp++ = '4';
#else
                *temp++ = 'l'; /* %l */
#endif
                format +=2;                         /* span the "tm" prefix */
                }
                else
                {
                    M4OSA_chrNCompare(format, (M4OSA_Char*)"aa",2, &iResult);
                    if (iResult != 0) /* à voir si ce n'est pas != 0 */
                    {
#ifdef M4OSA_64BITS_SUPPORTED
#ifdef M4OSA_FILE_POS_64_BITS_SUPPORTED
                        *temp++ = 'I'; /* %I64 */
                        *temp++ = '6';
                        *temp++ = '4';
#else
                        *temp++ = 'l';
#endif
#else
                        *temp++ = 'l';
#endif
                        format +=2;                         /* span the "aa" prefix */
                    }
                }
            }
        }

        M4OSA_chrSpan(format, otherPrefixChar, &nbChar);

        if(nbChar)
        {
            M4OSA_chrNCopy(temp,format,nbChar);      /* copy the other Prefix */
            format +=nbChar;
            temp   +=nbChar;
        }

        if(!*format)
        {
            break;
        }

        M4OSA_chrSpan(format, conversionChar, &nbChar);
        if(nbChar)
        {
            M4OSA_chrNCopy(temp,format,nbChar);
            format += nbChar;
            temp   += nbChar;
        }

        if(!*format)
        {
            break;
        }
   }

    err = M4OSA_chrSPrintf(strOut,strOutMaxLen,newFormat,marker);

   M4OSA_free((M4OSA_MemAddr32)newFormat);

   if (M4ERR_CHR_STR_OVERFLOW == err)
   {
       return M4ERR_STR_OVERFLOW;
   }
   else
   {
       return M4NO_ERROR;
   }
}

