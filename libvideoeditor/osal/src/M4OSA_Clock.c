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
 * @file         M4OSA_Clock.c
 * @brief        Clock related functions
 * @note         This file implements functions to manipulate clock
 ************************************************************************
*/

#include <sys/time.h>
#include <time.h>

#include "M4OSA_Debug.h"
#include "M4OSA_Clock.h"
#include "M4OSA_Memory.h"
#include "M4OSA_Types.h"




/**
 ************************************************************************
 * @brief      This function gets an absolute time to an unknown reference with
 *             a high precision.
 * @note       It means it can only be used to get a relative time by computing
 *             differences between to times.
 *             It is to the caller to allocate time. Time is expressed in
 *             timescale unit.
 *             M4OSA_ROLLOVER_CLOCK in M4OSA_Types.h must be configured with the rollover
 *             offset of this function.
 * @param      time: (IN/OUT) time
 * @param      timescale: (IN) The timescale (time unit per second)
 * @return     M4NO_ERROR: there is no error
 * @return     M4ERR_PARAMETER: at least one parameter is NULL
 * @return     M4WAR_TIMESCALE_TOO_BIG: the precision of the system clock is
 *             not
 *             compliant with the input timescale
 ************************************************************************
*/
M4OSA_ERR M4OSA_clockGetTime(M4OSA_Time* pTime, M4OSA_UInt32 timescale)
{
    struct timeval tv;
    struct timezone tz;
#ifdef M4OSA_64BITS_NOT_SUPPORTED
    M4OSA_UInt32 u32_time = 0;
    M4OSA_UInt32 u32_time_hi;
    M4OSA_UInt32 u32_time_lo;
    M4OSA_UInt32 u32_time_lh;
#else /* M4OSA_64BITS_SUPPORTED */
    M4OSA_Int64 i64_time = 0;
    M4OSA_Int64 i64_time_hi;
    M4OSA_Int64 i64_time_lo;
    M4OSA_Int64 i64_temp;
#endif /* M4OSA_64BITS_SUPPORTED */
    M4OSA_UInt32 factor;

    M4OSA_TRACE1_2("M4OSA_clockGetTime\t\tM4OSA_Time* 0x%x\tM4OSA_UInt32 %d",
                                                              pTime, timescale);

    M4OSA_DEBUG_IF2(M4OSA_NULL == pTime, M4ERR_PARAMETER,
                                     "M4OSA_clockGetTime: pTime is M4OSA_NULL");
    M4OSA_DEBUG_IF2(0 == timescale, M4ERR_PARAMETER,
                                          "M4OSA_clockGetTime: timescale is 0");

    factor = 1000000 / timescale;

    if(gettimeofday(&tv, &tz) == 0)
    {
#ifdef M4OSA_64BITS_NOT_SUPPORTED
        u32_time_lo = (tv.tv_sec & 0xFFFF) * timescale;
        u32_time_hi = (((tv.tv_sec >> 16) & 0xFFFF) * timescale) + ((u32_time_lo >> 16) & 0xFFFF);
        u32_time_lo &= 0xFFFF;
        u32_time_lo += tv.tv_usec / factor;
        u32_time_hi += ((u32_time_lo >> 16) & 0xFFFF);
        u32_time_lo &= 0xFFFF;
        u32_time = ((u32_time_hi & 0x7FFF) << 16) | u32_time_lo;
#else /* M4OSA_64BITS_SUPPORTED */
        tv.tv_usec /= factor;
        M4OSA_INT64_FROM_INT32_UINT32(i64_time_hi, 0, tv.tv_sec);
        M4OSA_INT64_FROM_INT32_UINT32(i64_time_lo, 0, tv.tv_usec);
        M4OSA_INT64_SCALAR_PRODUCT(i64_temp, i64_time_hi, timescale);
        M4OSA_INT64_ADD(i64_time, i64_temp, i64_time_lo);
#endif /* M4OSA_64BITS_SUPPORTED */
    }

#ifdef M4OSA_64BITS_NOT_SUPPORTED
    /* M4OSA_Time is signed, so we need to check the max value*/
    if (u32_time > M4OSA_INT32_MAX)
    {
        u32_time = u32_time - M4OSA_INT32_MAX;
    }

    *pTime = (M4OSA_Time)u32_time;

    if( timescale > 10000 )
    {
        return M4WAR_TIMESCALE_TOO_BIG;
    }
#else /* M4OSA_64BITS_SUPPORTED */
    *pTime = (M4OSA_Time)i64_time;

    if( timescale > 1000000 )
    {
        return M4WAR_TIMESCALE_TOO_BIG;
    }
#endif /* M4OSA_64BITS_SUPPORTED */

    return M4NO_ERROR;
}
