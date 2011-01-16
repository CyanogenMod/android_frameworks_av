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
 * @file   M4CLOCK.h
 * @brief  Clock and sleep functions types
 *
*************************************************************************
*/
#ifndef __M4CLOCK_H__
#define __M4CLOCK_H__

#include "M4OSA_Types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * Type of a function that returns time.
 */
typedef M4OSA_Double    (*M4CLOCK_getTime_fct) ( M4OSA_Void* pContext ) ;

/**
 * Type of a function that suspends a task for a certain amount of time.
 */
typedef M4OSA_Void        (*M4CLOCK_sleep_fct)    ( M4OSA_Void* pContext,\
                                                     M4OSA_UInt32 durationInMs ) ;

#ifdef __cplusplus
}
#endif

#endif /* __M4CLOCK_H__ */

