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
 * @file         M4OSA_Time.h
 * @ingroup      OSAL
 * @brief        Time macros
 * @note         This file defines time type and associated macros which must
 *               be used to manipulate time.
 ************************************************************************
*/

/* $Id: M4OSA_Time.h,v 1.2 2007/01/05 13:12:22 thenault Exp $ */

#ifndef M4OSA_TIME_H
#define M4OSA_TIME_H


#include "M4OSA_Types.h"


typedef M4OSA_Int64   M4OSA_Time;


/** This macro sets the unknown time value */
#ifdef M4OSA_64BITS_SUPPORTED
   #define M4OSA_TIME_SET_UNKNOWN(time) {time = 0x8000000000000000LL ;}
#endif /* M4OSA_64BITS_SUPPORTED */

#ifdef M4OSA_64BITS_COUPLE_INT
   #define M4OSA_TIME_SET_UNKNOWN(time) {\
      time.major = 0x80000000 ;\
      time.minor = 0x00000000 ;}
#endif /* M4OSA_64BITS_COUPLE_INT */

#ifdef M4OSA_64BITS_NOT_SUPPORTED
   #define M4OSA_TIME_SET_UNKNOWN(time) {time = 0x80000000;}
#endif   /* M4OSA_64BITS_NOT_SUPPORTED */


/** This macro returns 1 if the provided time is set to unknown time,
    and 0 else.*/
#ifdef M4OSA_64BITS_SUPPORTED

#define M4OSA_TIME_IS_UNKNOWN(time) (((M4OSA_UInt64)(time) == 0x8000000000000000LL) ? 1 : 0)

#elif defined M4OSA_64BITS_COUPLE_INT

#define M4OSA_TIME_IS_UNKNOWN(time)\
   (( (M4OSA_INT64_GET_HIGH32(time) == M4OSA_unknownTimeMajor)\
      &&(M4OSA_INT64_GET_LOW32(time) == M4OSA_unknownTimeMinor) ) ? 1:0)

#else /* M4OSA_64BITS_NOT_SUPPORTED */

#define M4OSA_TIME_IS_UNKNOWN(time) (((M4OSA_UInt32)(time) == 0x80000000) ? 1 : 0)

#endif


/** This macro affects time2 to time1.*/
#define M4OSA_TIME_SET(time1, time2)\
        M4OSA_INT64_SET(time1, time2)


/** This macro sets time from i32.*/
#define M4OSA_TIME_FROM_INT32(time, i32)\
        M4OSA_INT64_FROM_INT32(time, i32)


/** This macro sets time from i32 ui32.*/
#define M4OSA_TIME_FROM_INT32_UINT32(time, i32, ui32)\
        M4OSA_INT64_FROM_INT32_UINT32(time, i32, ui32)


/** This macro tests if time is positive*/
#define M4OSA_TIME_IS_POSITIVE(time)\
        M4OSA_INT64_IS_POSITIVE(time)


/** This macro sets time_out = -time_in*/
#define M4OSA_TIME_NEG(time_out, time_in)\
        M4OSA_INT64_NEG(time_out, time_in)


/** This macro sets time_out = |time_in|*/
#define M4OSA_TIME_ABS(time_out, time_in)\
        M4OSA_INT64_ABS(time_out, time_in)


/** This macro adds the 2 provided times (time1 and time2),
    and writes the result in result. Both times must have the same timescale.*/
#define M4OSA_TIME_ADD(result, time1, time2)\
        M4OSA_INT64_ADD(result, time1, time2)


/** This macro subs the 2 provided times (time1 and time2),
    and writes the result in result.*/
#define M4OSA_TIME_SUB(result, time1, time2)\
        M4OSA_INT64_SUB(result, time1, time2)


/** This macro does a scalar product (result = time*value),
    and writes the result in result.*/
#define M4OSA_TIME_SCALAR_PRODUCT(result, time, value)\
        M4OSA_INT64_SCALAR_PRODUCT(result, time, value)


/** This macro does a scalar division (result= time / value),
    and writes the result in result.*/
#define M4OSA_TIME_SCALAR_DIVISION(result, time, value)\
        M4OSA_INT64_SCALAR_DIVISION(result, time, value)


/** This macro updates the time to the oldTimeScale to the newTimeScale. The
    result (the nearest rounded to the min value) is stored in result value. */
#define M4OSA_TIME_CHANGE_TIMESCALE(result, time, oldTimeScale, newTimeScale)\
      { M4OSA_Time t_tempTime1, t_tempTime2, t_tempTime3;\
        M4OSA_Int32 i32_quotient = newTimeScale/oldTimeScale;\
        M4OSA_Int32 i32_rest = newTimeScale%oldTimeScale;\
        M4OSA_INT64_SCALAR_PRODUCT(t_tempTime1, time, i32_quotient);\
        M4OSA_INT64_SCALAR_PRODUCT(t_tempTime2, time, i32_rest);\
        M4OSA_INT64_SCALAR_DIVISION(t_tempTime3, t_tempTime2, oldTimeScale);\
        M4OSA_INT64_ADD(result, t_tempTime1, t_tempTime3); }


/** This macro tests the 2 provided times (time1 & time2).
    The result is either:
  * @arg  1: if time1 is bigger than time2
  * @arg  0: if time2 is equal to time2
  * @arg -1: if time1 is smaller than time2  */
#define M4OSA_TIME_COMPARE(time1, time2)\
        M4OSA_INT64_COMPARE(time1, time2)


/** This macro converts a time with a time scale to millisecond.
    The result is a M4OSA_Double*/
#define M4OSA_TIME_TO_MS(result, time, timescale)\
      { M4OSA_INT64_TO_DOUBLE(result, time);\
        result = (1000*result)/((M4OSA_Double)timescale); }


/** This macro converts a millisecond time to M4OSA_Time with the provided
    timescale. The result (the nearest rounded to the min value) is stored
    in time value.*/
#define M4OSA_MS_TO_TIME(time, timescale, ms)\
      {M4OSA_INT64_FROM_DOUBLE(time, (ms*((M4OSA_Double)(timescale))/1000.0));}


#endif /*M4OSA_TIME_H*/

