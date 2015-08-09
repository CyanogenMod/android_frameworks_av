/*****************************************************************************/
/*                                                                           */
/*                               FLAC PARSER(FLACP)                          */
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
/*  File Name         : flacp_metadata.h                                     */
/*                                                                           */
/*  Description       : This file contains structures and functions related  */
/*                      to metadata parsing                                  */
/*                                                                           */
/*  List of Functions : None                                                 */
/*                                                                           */
/*  Issues / Problems : None                                                 */
/*                                                                           */
/*  Revision History  :                                                      */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         28 03 2009   Ittiam          Draft                                */
/*                                                                           */
/*****************************************************************************/

#ifndef FLACP_METADATA_H
#define FLACP_METADATA_H

/*****************************************************************************/
/* C linkage specifiers for C++ declarations                                 */
/*****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif  /* __cplusplus */
#define META_DATA_SUPPORT
/*****************************************************************************/
/* Enums	                                                                 */
/*****************************************************************************/

/* Different metadata blocks that are currently defined by flac file format */
typedef enum
{
	STREAMINFO_BLOCK = 0,
	PADDING_BLOCK,
	APPLICATION_BLOCK,
	SEEKTABLE_BLOCK,
	VORBIS_COMMENT_BlOCK,
	CUESHEET_BLOCK,
	PICTURE_BLOCK,
	UNKNOWN_BLOCK,
	INVALID_BLOCK
} FLACP_BLOCK_TYPE_T;

/*****************************************************************************/
/* Structures                                                                */
/*****************************************************************************/

/* This stucture contains offset values for floowing metadata blocks */
typedef struct
{
	UWORD32 comment_offset;	     /* Vorbis comment offset location    */
	UWORD32 comment_block_size;  /* Size of the comment data block    */
	UWORD32 frame_data_offset;   /* First frame data offset location  */
} flacp_block_header_offset_t;

/* This structure contains frame header info, that is useful for seeking */
typedef struct
{
    UWORD32 frame_size;   /* Size of the frame in samples	  */
    UWORD64 first_sample; /* First sample Number in the frame */
} frame_header_info_t;

/******************************************************************************/
/* Extern Function Declarations                                               */
/******************************************************************************/

/* Function to parse stream info metadata block */
extern WORD32 flacp_parse_stream_metadata(
									   IN    void				 *buf,
									   IN    WORD64 			 file_len,
									   IN    flacp_cb_funcs_t    *cb_funcs,
									   INOUT WORD64				 *fp_offset,
									   OUT	 UWORD8				 *last_block,
									   OUT   flacp_stream_info_t *stream_info);

/* Function to check for other metadata blocks */
#ifdef META_DATA_SUPPORT

extern WORD32 flacp_check_other_metadata_blocks(
								IN    void			   *scratch_buf,
								IN    WORD64		   file_len,
								IN    flacp_cb_funcs_t *cb_funcs,
								IN    UWORD8		   last_block,
								INOUT WORD64		   *fp_offset,
								OUT flacp_seek_table_attr_t     *seek_table,
								OUT flacp_seek_details_t        *seek_details,
								OUT flacp_block_header_offset_t *header_offset,
								OUT flacp_metadata_info_t		*meta_data);


#else

	extern WORD32 flacp_check_other_metadata_blocks(
							   IN    void			  *buf,
							   IN    WORD64 		  file_len,
							   IN    flacp_cb_funcs_t *cb_funcs,
							   IN	 UWORD8			  last_block,
							   INOUT WORD64			  *fp_offset,
							   OUT   flacp_seek_table_attr_t   *seek_table,
							   OUT flacp_seek_details_t        *seek_details,
							   OUT flacp_block_header_offset_t *header_offset);
#endif /*META_DATA_SUPPORT*/

/*****************************************************************************/
/* C linkage specifiers for C++ declarations                                 */
/*****************************************************************************/

#ifdef __cplusplus
}                       /* End of extern "C" { */
#endif  /* __cplusplus */

#endif /* End of FLACP_METADATA_H */
