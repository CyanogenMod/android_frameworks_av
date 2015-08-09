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
/*  File Name         : flacp_handle.h                                       */
/*                                                                           */
/*  Description       : This file contains the necessary constants, enums,   */
/*                      structures and API declarations for the FLAC Parser  */
/*                      Handle                                               */
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

#ifndef FLACP_HANDLE_H
#define FLACP_HANDLE_H

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

/* Library version */
#define FLACP_VERSION		   "FLACP_v1.0"

/* File header FOURCC identifier */
#define FLAC_HEADER_ID		   FOURCC('f', 'L', 'a', 'C')

/* File header id size */
#define FLACP_FILE_HEADER_SIZE (4)

/* Minimum block size to be occured in FLAC file */
#define MIN_BLOCK_SIZE			(16)

/* MD5 array size */
#define FLACP_MD5_SIGN_LENGTH 	  (16)
#define META_DATA_SUPPORT
/*****************************************************************************/
/* Structures                                                                */
/*****************************************************************************/

/* This structure is used to initialize the parser and is used in all frame  */
/* and tag related API calls		                                         */
struct flacp_t
{		
	flacp_stream_info_t         stream_info;   /* Stream info metadata block */
	flacp_block_header_offset_t header_offset; /* Header offset structure	 */
	flacp_seek_details_t        seek_details;  /* Seek metadata block info   */
	flacp_seek_table_attr_t     seek_table;    /* Seek table attributes		 */
	flacp_frame_details_t       frame_details; /* Frame details structure    */

	UWORD8  stream_flag;		/* Flag to indicate stream info present		 */
	WORD64  file_len;			/* Length of the file in bytes				 */
	WORD64  audio_data_offset;  /* Current file offset location to read 	 */
								/* audio data from the file					 */
	UWORD8  *scratch_buf;		/* Scratch buffer used to read stream info   */

	flacp_attr_t	 attr;          /* FLAC Parser attributes 				 */
	flacp_cb_funcs_t cb_funcs;      /* Callback functions		    		 */
	memory_handle_t	 memory_handle; /* Handles the flacp handle memory		 */
#ifdef META_DATA_SUPPORT
	flacp_metadata_info_t		meta_data; /*handles all metadata related like picture, vorbis comment details*/
	
#endif
};

/*****************************************************************************/
/* C linkage specifiers for C++ declarations                                 */
/*****************************************************************************/

#ifdef __cplusplus
}                       /* End of extern "C" { */
#endif  /* __cplusplus */

#endif /* FLACP_HANDLE_H */
