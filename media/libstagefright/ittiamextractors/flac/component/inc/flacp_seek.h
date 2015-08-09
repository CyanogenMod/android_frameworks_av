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
/*  File Name         : flacp_seek.h                                         */
/*                                                                           */
/*  Description       : This file contains utility structures and function   */
/*                      declarations related to seek functionality           */
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

#ifndef FLACP_SEEK_H
#define FLACP_SEEK_H

/*****************************************************************************/
/* C linkage specifiers for C++ declarations                                 */
/*****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif  /* __cplusplus */

/*****************************************************************************/
/* Structures                                                                */
/*****************************************************************************/

/* This structure contains SEEKTABLE metadata block details, if present in 	 */
/* the input file															 */
typedef struct
{
	UWORD32 seek_offset;     /* Seek details metadata block offset */
	UWORD32 seek_block_size; /* Seek details metadata block size   */
} flacp_seek_details_t;

/* This structure contains seek table details */
typedef struct
{
	UWORD8  complete_seek_table; /* Flag to indicate, whether complete seek  */
								 /* details are read or not. If complete seek*/
								 /* details are not available, then parser   */
								 /* will read seek details from the file 	 */
								 /* multiple times, depending on size of the */
								 /* memory provided for seek table buffer	 */
	UWORD32 seek_table_size;     /* Size of the memory provided to store seek*/
							     /* details available in the input file		 */
	void	*seek_table_buf;	 /* Buffer used to store seek details that 	 */
							     /* available in the input file              */
} flacp_seek_table_attr_t;

/* This structure contains seektable metadata block entry fields */
typedef struct
{
	UWORD64 sample_number;  /* The sample number of the target frame		 */
	WORD64  stream_offset;  /* The offset, in bytes, of the target frame 	 */
                            /* with respect to beginning of the first frame  */
	UWORD16 frame_samples;  /* The number of samples in the target frame	 */
} flacp_metadata_seekpoint_t;

/* This structure is used to store last frame header details, which are used */
/* to compare with the target sample number									 */
typedef struct
{
	UWORD64 first_sample;  /* First sample number of the frame */
	WORD64  sample_offset; /* Offset of the sample number	   */
} flacp_frame_details_t;

/******************************************************************************/
/* Extern Function Declarations                                               */
/******************************************************************************/


/* Function to find upper and lower boundaries from seek table entries to the*/
/* requested sample number													 */
extern WORD32 parse_seektable_for_bounds(
								IN  flacp_seek_details_t	    *seek_details,
								IN  UWORD64					    sample_num,
								IN  flacp_cb_funcs_t			*cb_funcs,
								IN  flacp_seek_table_attr_t	    *seek_table,
								IN  WORD64						frame_offset,
								OUT flacp_metadata_seekpoint_t  *lower_bound,
								OUT flacp_metadata_seekpoint_t  *upper_bound);

/* Function to search for target sample number in the input file using binary*/
/* search between lower and upper bounds 									 */
extern WORD32 flacp_binary_search(
						   IN UWORD64 sample_num, IN WORD64 file_len,
						   IN flacp_cb_funcs_t *cb_funcs, IN void *temp_buf,
						   IN flacp_metadata_seekpoint_t lower_bound,
						   IN flacp_metadata_seekpoint_t upper_bound,
						   IN flacp_frame_details_t      *frame_details,
						   IN flacp_stream_info_t		 *stream_info,
						   OUT UWORD8 *entry_found, OUT WORD64  *offset);

/*****************************************************************************/
/* C linkage specifiers for C++ declarations                                 */
/*****************************************************************************/

#ifdef __cplusplus
}                       /* End of extern "C" { */
#endif  /* __cplusplus */

#endif /* End of FLACP_SEEK_H */
