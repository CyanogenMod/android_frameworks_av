/*****************************************************************************/
/*                                                                           */
/*                              FLAC Parser (FLACP)                          */
/*                     ITTIAM SYSTEMS PVT LTD, BANGALORE                     */
/*                             COPYRIGHT(C) 2009                             */
/*                                                                           */
/*  This program  is  proprietary to  Ittiam  Systems  Private  Limited  and */
/*  is protected under Indian  Copyright Law as an unpublished work. Its use */
/*  and  disclosure  is  limited by  the terms  and  conditions of a license */
/*  agreement. It may not be copied or otherwise  reproduced or disclosed to */
/*  persons outside the licensee's organization except in accordance with the*/
/*  terms  and  conditions   of  such  an  agreement.  All  copies  and      */
/*  reproductions shall be the property of Ittiam Systems Private Limited and*/
/*  must bear this notice in its entirety.                                   */
/*                                                                           */
/*****************************************************************************/

/*****************************************************************************/
/*                                                                           */
/*  File Name         : flacp_errno.h                                        */
/*                                                                           */
/*  Description       : This file contains the error numbers                 */
/*                                                                           */
/*  List of Functions : None                                                 */
/*                                                                           */
/*  Issues / Problems : None                                                 */
/*                                                                           */
/*  Revision History  :                                                      */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         24 03 2009   Ittiam          Draft                                */
/*                                                                           */
/*****************************************************************************/

#ifndef FLACP_ERRNO_H
#define FLACP_ERRNO_H

/*****************************************************************************/
/* C linkage specifiers for C++ declarations                                 */
/*****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif  /* __cplusplus */

/*****************************************************************************/
/* Constants                                                                 */
/*****************************************************************************/

#define FLACP_ERROR_BASE				 0x27000

#define FLACP_ERROR		 			     (FLACP_ERROR_BASE + 0x01)
#define FLACP_INVALID_INPUT              (FLACP_ERROR_BASE + 0x02)
#define FLACP_INVALID_FILE               (FLACP_ERROR_BASE + 0x03)
#define FLACP_FILE_LEN_ERROR             (FLACP_ERROR_BASE + 0x04)
#define FLACP_FILE_SEEK_ERROR            (FLACP_ERROR_BASE + 0x05)
#define FLACP_FILE_READ_ERROR		     (FLACP_ERROR_BASE + 0x06)
#define FLACP_MEM_ALLOC_ERROR            (FLACP_ERROR_BASE + 0x07)
#define FLACP_STREAM_INFO_ABSENT	     (FLACP_ERROR_BASE + 0x08)
#define FLACP_NO_FRAMES_FOUND	         (FLACP_ERROR_BASE + 0x09)
#define FLACP_INVALID_MINIMUM_BLOCK_SIZE (FLACP_ERROR_BASE + 0x0A)
#define FLACP_END_OF_FILE 			     (FLACP_ERROR_BASE + 0x0B)
#define FLACP_SAMPLE_NUMBER_NOT_FOUND    (FLACP_ERROR_BASE + 0x0C)
#define FLACP_SEEK_OUT_OF_RANGE	   	     (FLACP_ERROR_BASE + 0x0D)

/******************************************************************************/
/* C linkage specifiers for C++ declarations                                  */
/******************************************************************************/

#ifdef __cplusplus
}                       /* End of extern "C" { */
#endif  /* __cplusplus */

#endif /* FLACP_ERRNO_H */
