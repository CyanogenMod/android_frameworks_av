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
 * @file         M4OSA_Types.h
 * @ingroup      OSAL
 * @brief        Abstraction types for Android
 * @note         This file redefines basic types which must be
 *               used to declare any variable.
************************************************************************
*/


#ifndef M4OSA_TYPES_H
#define M4OSA_TYPES_H

#include <ctype.h>
#include "M4OSA_Export.h"
#ifdef __cplusplus
extern "C" {
#endif

/*#define M4OSA_64BITS_SUPPORTED     */  /* means long long is used        */
/*#define M4OSA_64BITS_COUPLE_INT    */    /* means couple int32 is used    */
#define M4OSA_64BITS_NOT_SUPPORTED      /* means Int32 is used            */


typedef signed char     M4OSA_Bool;
typedef unsigned char   M4OSA_UInt8;
typedef signed char     M4OSA_Int8;
typedef unsigned short  M4OSA_UInt16;
typedef signed short    M4OSA_Int16;
typedef unsigned long   M4OSA_UInt32;
typedef signed long     M4OSA_Int32;

typedef signed char     M4OSA_Char;
typedef unsigned char   M4OSA_UChar;

typedef double          M4OSA_Double;
typedef float           M4OSA_Float;

typedef unsigned char   M4OSA_WChar;

typedef void            M4OSA_Void;

typedef struct
{
   M4OSA_Int32   high;
   M4OSA_Int32   low;
} M4OSA_CoupleInt32;

#ifdef M4OSA_64BITS_SUPPORTED
typedef signed long long M4OSA_Int64;
typedef unsigned long long M4OSA_UInt64;
#endif

#ifdef M4OSA_64BITS_COUPLE_INT
typedef struct
{
   M4OSA_Int32 major;
   M4OSA_UInt32 minor;
} M4OSA_Int64;
typedef struct
{
   M4OSA_UInt32 major;
   M4OSA_UInt32 minor;
} M4OSA_UInt64;
#endif

#ifdef M4OSA_64BITS_NOT_SUPPORTED
typedef M4OSA_Int32 M4OSA_Int64;
typedef M4OSA_UInt32 M4OSA_UInt64;
#endif

/* Min & max definitions*/
#define M4OSA_UINT8_MIN                  0
#define M4OSA_UINT8_MAX                255

#define M4OSA_UINT16_MIN                 0
#define M4OSA_UINT16_MAX             65535

#define M4OSA_UINT32_MIN                 0
#define M4OSA_UINT32_MAX        0xFFFFFFFF

#define M4OSA_INT8_MIN                -128
#define M4OSA_INT8_MAX                 127

#define M4OSA_INT16_MIN             -32768
#define M4OSA_INT16_MAX              32767

#define M4OSA_INT32_MIN       (-0x7FFFFFFF-1)
#define M4OSA_INT32_MAX         0x7FFFFFFF

#define M4OSA_CHAR_MIN                -128
#define M4OSA_CHAR_MAX                 127

#define M4OSA_UCHAR_MIN                  0
#define M4OSA_UCHAR_MAX                255

#ifdef M4OSA_64BITS_NOT_SUPPORTED

#define M4OSA_UINT64_MIN        M4OSA_UINT32_MIN
#define M4OSA_UINT64_MAX        M4OSA_UINT32_MAX
#define M4OSA_INT64_MIN          M4OSA_INT32_MIN
#define M4OSA_INT64_MAX          M4OSA_INT32_MAX

#else /* M4OSA_64BITS_NOT_SUPPORTED*/

#define M4OSA_UINT64_MIN                       0
#define M4OSA_UINT64_MAX      0xFFFFFFFFFFFFFFFFLL
#define M4OSA_INT64_MIN       0x8000000000000000LL
#define M4OSA_INT64_MAX       0x7FFFFFFFFFFFFFFFLL

#endif /* M4OSA_64BITS_NOT_SUPPORTED*/

#define M4OSA_NULL                     0x00
#define M4OSA_TRUE                     0x01
#define M4OSA_FALSE                    0x00
#define M4OSA_WAIT_FOREVER       0xffffffff

#define M4OSA_CONST                   const
#define M4OSA_INLINE                 inline

/* Rollover offset of the clock */
/* This value must be the one of M4OSA_clockGetTime */
#define M4OSA_CLOCK_ROLLOVER           M4OSA_INT32_MAX

typedef void*                M4OSA_Context;


/** It is a unique ID for each core component*/
typedef  M4OSA_UInt16 M4OSA_CoreID;


/* Macro to support big endian and little endian platform */

/* to translate a 16 bits to its Big Endian value*/
#define M4OSA_INT16_TO_BE(ui16_host) ((((ui16_host) & (M4OSA_UInt16) 0x00ff) << 8) | \
                                      (((ui16_host) & (M4OSA_UInt16) 0xff00) >> 8) )

/* to translate a 32 bits to its Big Endian value */
#define M4OSA_INT32_TO_BE(ui32_host) ((((ui32_host) & (M4OSA_UInt32) 0x000000ff) << 24) | \
                                      (((ui32_host) & (M4OSA_UInt32) 0x0000ff00) <<  8) | \
                                      (((ui32_host) & (M4OSA_UInt32) 0x00ff0000) >>  8) | \
                                      (((ui32_host) & (M4OSA_UInt32) 0xff000000) >>  24))

/* to translate a 64 bits to its Big Endian value */
#define M4OSA_INT64_TO_BE(ui64_host) ((((ui64_host) & (M4OSA_UInt64) 0x00000000000000ff) << 56) | \
                                      (((ui64_host) & (M4OSA_UInt64) 0x000000000000ff00) << 40) | \
                                      (((ui64_host) & (M4OSA_UInt64) 0x0000000000ff0000) << 24) | \
                                      (((ui64_host) & (M4OSA_UInt64) 0x00000000ff000000) <<  8) | \
                                      (((ui64_host) & (M4OSA_UInt64) 0x000000ff00000000) >>  8) | \
                                      (((ui64_host) & (M4OSA_UInt64) 0x0000ff0000000000) >> 24) | \
                                      (((ui64_host) & (M4OSA_UInt64) 0x00ff000000000000) >> 40) | \
                                      (((ui64_host) & (M4OSA_UInt64) 0xff00000000000000) >> 56))

/* to translate a Big Endian 16 bits to its host representation */
#define M4OSA_BE_TO_INT16(ui16_net) ((((ui16_net) & (M4OSA_UInt16) 0x00ff) << 8) | \
                                     (((ui16_net) & (M4OSA_UInt16) 0xff00) >> 8) )

/* to translate a Big Endian 32 bits to its host representation*/
#define M4OSA_BE_TO_INT32(ui32_net) ((((ui32_net) & (M4OSA_UInt32) 0x000000ff) << 24) | \
                                     (((ui32_net) & (M4OSA_UInt32) 0x0000ff00) <<  8) | \
                                     (((ui32_net) & (M4OSA_UInt32) 0x00ff0000) >>  8) | \
                                     (((ui32_net) & (M4OSA_UInt32) 0xff000000) >>  24))

/* to translate a Big Endian 64 bits to its host representation */
#define M4OSA_BE_TO_INT64(ui64_net) ((((ui64_net) & (M4OSA_UInt64) 0x00000000000000ff) << 56) | \
                                     (((ui64_net) & (M4OSA_UInt64) 0x000000000000ff00) << 40) | \
                                     (((ui64_net) & (M4OSA_UInt64) 0x0000000000ff0000) << 24) | \
                                     (((ui64_net) & (M4OSA_UInt64) 0x00000000ff000000) <<  8) | \
                                     (((ui64_net) & (M4OSA_UInt64) 0x000000ff00000000) >>  8) | \
                                     (((ui64_net) & (M4OSA_UInt64) 0x0000ff0000000000) >> 24) | \
                                     (((ui64_net) & (M4OSA_UInt64) 0x00ff000000000000) >> 40) | \
                                     (((ui64_net) & (M4OSA_UInt64) 0xff00000000000000) >> 56))

/* to translate a 16 bits to its Little Endian value*/
#define M4OSA_INT16_TO_LE(ui16_host) (ui16_host)

/* to translate a 32 bits to its Little Endian value */
#define M4OSA_INT32_TO_LE(ui32_host) (ui32_host)

/* to translate a 64 bits to its Little Endian value */
#define M4OSA_INT64_TO_LE(ui64_host) (ui64_host)

/* to translate a Little Endian 16 bits to its host representation */
#define M4OSA_LE_TO_INT16(ui16_net) (ui16_net)

/* to translate a Little Endian 32 bits to its host representation*/
#define M4OSA_LE_TO_INT32(ui32_net) (ui32_net)

/* to translate a Little Endian 64 bits to its host representation */
#define M4OSA_LE_TO_INT64(ui64_net) (ui64_net)


/* Macro to manipulate M4OSA_Int32*/
#define M4OSA_INT32_SET(i32_out, i32_in)\
   { i32_out = i32_in; }

#define M4OSA_INT32_ADD(i32_result, i32_a, i32_b)\
   { i32_result = (i32_a) + (i32_b); }

#define M4OSA_INT32_SUB(i32_result, i32_a, i32_b)\
   { i32_result = (i32_a) - (i32_b); }

#define M4OSA_INT32_SCALAR_PRODUCT(i32_result, i32_a, i32_value)\
   { i32_result = (i32_a) * (i32_value); }

#define M4OSA_INT32_SCALAR_DIVISION(i32_result, i32_a, i32_value)\
   { i32_result = (i32_a) / (i32_value); }

#define M4OSA_INT32_COMPARE(i32_a, i32_b)\
   ( ((i32_a) == (i32_b)) ? 0 : ( ((i32_a) > (i32_b)) ? 1 : -1) )

#define M4OSA_INT32_FROM_INT32(i32_result, i32_value)\
   { i32_result = (M4OSA_Int32)(i32_value); }

#define M4OSA_INT32_FROM_INT32_UINT32(i32_result, i32_high, ui32_low)\
   { i32_result = (M4OSA_Int32)(ui32_low); }

#define M4OSA_INT32_GET_LOW32(i32_value) ((M4OSA_Int32)(i32_value))

#define M4OSA_INT32_GET_HIGH32(i32_value) (0)

#define M4OSA_INT32_IS_POSITIVE(i32_value) ((i32_value) >= 0)

#define M4OSA_INT32_NEG(i32_result, i32_value)\
   { i32_result = -(i32_value); }

#define M4OSA_INT32_ABS(i32_result, i32_value)\
   { if ((i32_value) > 0) { i32_result = i32_value; }\
     else                 { i32_result = -(i32_value); } }

#define M4OSA_INT32_LEFT_SHIFT(i32_result, i32_value, ui32_nbPos)\
   { i64_result = (((ui32_nbPos)>0x1F)?0:((i64_value)<<(ui32_nbPos))); }

#define M4OSA_INT32_RIGHT_SHIFT(i32_result, i32_value, ui32_nbPos)\
   { i64_result = (((ui32_nbPos)>0x1F)?0:((i64_value)>>(ui32_nbPos))); }

#define M4OSA_INT32_TO_DOUBLE(f_result, i32_value)\
   { f_result = (M4OSA_Double)(i32_value); }

#define M4OSA_INT32_FROM_DOUBLE(i32_result, f_value)\
   { i32_result = (M4OSA_Int32)(f_value); }


#ifdef M4OSA_64BITS_SUPPORTED

/* Macro to manipulate M4OSA_Int64*/
#define M4OSA_INT64_SET(i64_out, i64_in) { i64_out = i64_in; }

#define M4OSA_INT64_ADD(i64_result, i64_a, i64_b)\
   { i64_result = (i64_a) + (i64_b); }

#define M4OSA_INT64_SUB(i64_result, i64_a, i64_b)\
   { i64_result = (i64_a) - (i64_b); }

#define M4OSA_INT64_SCALAR_PRODUCT(i64_result, i64_a, i32_value)\
   { i64_result = (i64_a) * (i32_value); }

#define M4OSA_INT64_SCALAR_DIVISION(i64_result, i64_a, i32_value)\
   { i64_result = (i64_a) / (i32_value); }

#define M4OSA_INT64_COMPARE(i64_a, i64_b)\
   ( ((i64_a) == (i64_b)) ? 0 : ( ((i64_a) > (i64_b)) ? 1 : -1) )\

#define M4OSA_INT64_FROM_INT32(i64_result, i32_value)\
   { i64_result = (M4OSA_Int64)(i32_value); }

#define M4OSA_INT64_FROM_INT32_UINT32(i64_result, i32_high, ui32_low)\
   { i64_result = (i32_high); i64_result = (i64_result<<32)+(ui32_low); }

#define M4OSA_INT64_GET_LOW32(i64_value)\
   ((M4OSA_Int32)((i64_value) & 0xFFFFFFFF))

#define M4OSA_INT64_GET_HIGH32(i64_value)\
   ((M4OSA_Int32)(((i64_value) >> 32) & 0xFFFFFFFF))

#define M4OSA_INT64_IS_POSITIVE(i64_value) (((i64_value)>=0)?1:0)

#define M4OSA_INT64_NEG(i64_result, i64_value)\
   { i64_result = -(i64_value); }

#define M4OSA_INT64_ABS(i64_result, i64_value)\
   { if (M4OSA_INT64_IS_POSITIVE(i64_value)) { i64_result = i64_value; }\
     else { M4OSA_INT64_NEG(i64_result, i64_value); } }

#define M4OSA_INT64_LEFT_SHIFT(i64_result, i64_value, ui32_nbPos)\
   { i64_result = (((ui32_nbPos)>0x3F)?0:((i64_value)<<(ui32_nbPos))); }

#define M4OSA_INT64_RIGHT_SHIFT(i64_result, i64_value, ui32_nbPos)\
   { i64_result = (((ui32_nbPos)>0x3F)?0:((i64_value)>>(ui32_nbPos))); }

#define M4OSA_INT64_TO_DOUBLE(f_result, i64_value)\
   { f_result = (M4OSA_Double)(i64_value); }

#define M4OSA_INT64_FROM_DOUBLE(i64_result, f_value)\
   { i64_result = (M4OSA_Int64)(f_value); }

#endif   /*M4OSA_64BITS_SUPPORTED*/


#ifdef M4OSA_64BITS_NOT_SUPPORTED

#define M4OSA_INT64_SET(i64_out, i64_in)\
        M4OSA_INT32_SET(i64_out, i64_in)

#define M4OSA_INT64_ADD(i64_result, i64_a, i64_b)\
        M4OSA_INT32_ADD(i64_result, i64_a, i64_b)

#define M4OSA_INT64_SUB(i64_result, i64_a, i64_b)\
        M4OSA_INT32_SUB(i64_result, i64_a, i64_b)

#define M4OSA_INT64_SCALAR_PRODUCT(i64_result, i64_a, i32_value)\
        M4OSA_INT32_SCALAR_PRODUCT(i64_result, i64_a, i32_value)

#define M4OSA_INT64_SCALAR_DIVISION(i64_result, i64_a, i32_value)\
        M4OSA_INT32_SCALAR_DIVISION(i64_result, i64_a, i32_value)

#define M4OSA_INT64_COMPARE(i64_a, i64_b)\
        M4OSA_INT32_COMPARE(i64_a, i64_b)

#define M4OSA_INT64_FROM_INT32(i64_result, i32_value)\
        M4OSA_INT32_FROM_INT32(i64_result, i32_value)

#define M4OSA_INT64_FROM_INT32_UINT32(i64_result, i32_high, ui32_low)\
        M4OSA_INT32_FROM_INT32_UINT32(i64_result, i32_high, ui32_low)

#define M4OSA_INT64_GET_LOW32(i64_value)\
        M4OSA_INT32_GET_LOW32(i64_value)

#define M4OSA_INT64_GET_HIGH32(i64_value)\
        M4OSA_INT32_GET_HIGH32(i64_value)

#define M4OSA_INT64_IS_POSITIVE(i64_value)\
        M4OSA_INT32_IS_POSITIVE(i64_value)

#define M4OSA_INT64_NEG(i64_result, i64_value)\
        M4OSA_INT32_NEG(i64_result, i64_value)

#define M4OSA_INT64_ABS(i64_result, i64_value)\
        M4OSA_INT32_ABS(i64_result, i64_value)

#define M4OSA_INT64_LEFT_SHIFT(i64_result, i64_value, ui32_nbPositions)\
        M4OSA_INT32_LEFT_SHIFT(i64_result, i64_value, ui32_nbPositions)

#define M4OSA_INT64_RIGHT_SHIFT(i64_result, i64_value, ui32_nbPositions)\
        M4OSA_INT32_RIGHT_SHIFT(i64_result, i64_value, ui32_nbPositions)

#define M4OSA_INT64_TO_DOUBLE(f_result, i64_value)\
        M4OSA_INT32_TO_DOUBLE(f_result, i64_value)

#define M4OSA_INT64_FROM_DOUBLE(i64_result, f_value)\
        M4OSA_INT32_FROM_DOUBLE(i64_result, f_value)

#endif /*M4OSA_64BITS_NOT_SUPPORTED*/


#ifdef __cplusplus
}
#endif

#endif /*M4OSA_TYPES_H*/

