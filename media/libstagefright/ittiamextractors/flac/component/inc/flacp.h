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
/*  File Name         : flacp.h                                              */
/*                                                                           */
/*  Description       : This file contains the necessary constants, enums,   */
/*                      structures and API declarations for the FLAC Parser  */
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

#ifndef FLACP_H
#define FLACP_H

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

/* Value returned by the FLAC Parser for a successful API */
#define FLACP_SUCCESS	      	  (0)

/* Size of the FLAC Parser handle required for every instance of FLAC Parser */
//#define FLACP_HANDLE_SIZE     	  (512)
#define FLACP_HANDLE_SIZE     	  (1024) /*changed since addition of metadata requires extra memory*/

/* File seek origins. To be used by file seek call back function */
#define FLACP_FILE_SEEK_SET   	  (0)
#define FLACP_FILE_SEEK_CUR   	  (1)
#define FLACP_FILE_SEEK_END   	  (2)

#define META_DATA_SUPPORT
#define MAX_VORBIS_COMMENT 32
/*****************************************************************************/
/* Structures                                                                */
/*****************************************************************************/

/* FLAC Parser handle stucture */
typedef struct flacp_t flacp_t;

/* This structure is used by system to provide attributes required for FLAC  */
/* Parser.                                                            		 */
typedef struct
{
    UWORD32 reserved; /* No attributes required currently. Field added for   */
                      /* extensibility                                       */
} flacp_attr_t;

/* Callback functions */
typedef struct
{
    /* Callback function called by FLACP whenever memory is to be allocated  */
    void *(*flacp_alloc)(IN void *flacp_al_params, IN UWORD32 size);

    /* flacp_alloc parameters - which will be sent as the first argument     */
    /* in the flacp_alloc call back function.                                */
    void *flacp_al_params;

	/* Callback function called by FLACP whenever allocated memory is to be  */
    /* freed                                                                 */
    void (*flacp_free )(IN void *flacp_fr_params,IN void  *mem_addr);

    /* flacp_free parameters - which will be sent as the first argument      */
    /* in the flacp_free call back function.                                 */
    void *flacp_fr_params;

    /* Callback function called by FLACP whenever file data is to be read    */
    WORD32 (*flacp_file_read) (IN  void    *flacp_rd_params,
                               IN  UWORD32 bytes_to_be_read,
                               IN  void    *buf,
                               OUT UWORD32 *bytes_read);

    /* File read parameters - which will be sent as the first argument       */
    /* in the flacp_file_read callback function .                            */
    void *flacp_rd_params;

    /* Callback function called by FLACP whenever file handle position is to */
    /* be changed                                                            */
    WORD32 (*flacp_file_seek) (IN void   *flacp_sk_params,
                               IN WORD64 offset,
                               IN WORD32 seek_origin);

    /* File seek parameters - which will be sent as the first argument       */
    /* in the flacp_file_seek callback function                              */
    void *flacp_sk_params;

	/* Callback function called by FLACP whenever file len is to be queried  */
    WORD32 (*flacp_file_length) (IN  void    *flacp_le_params,
                                 OUT UWORD64 *length);

    /* File length parameters - which will be sent as the first argument     */
    /* in the flacp_file_length callback function                            */
    void *flacp_le_params;

    /* Callback function called by FLACP whenever memory is to be allocated  */
	/* for seek table buffer. This callback can allocate less than the	     */
	/* requested memory, but it should allocate minimum memory  of 18 bytes. */
	/* The memory allocated by system will be given to Parser through seek   */
	/* table size parameter												     */
	void *(*flacp_seek_table_buf_alloc)(IN    void    *flacp_ta_params,
								    INOUT UWORD32 *seektable_size);

	/* Seek table buf alloc parameters - which will be sent as the first	 */
	/* argument in the flacp_seek_table_buf_alloc call back function.        */
	void *flacp_ta_params;

	/* Callback function called by FLACP whenever allocated memory for seek  */
    /* table buffer is required to free                                      */
    void (*flacp_seek_table_buf_free)(IN void *flacp_tf_params,
								   IN void *seektable_buf);

    /* Seek table buf free parameters - which will be sent as the first      */
	/* argument in the flacp_seek_table_buf_free call back function.         */
    void *flacp_tf_params;

} flacp_cb_funcs_t;

/* This structure used to provide information available in STREAMINFO   	 */
/* metadata block to system.												 */
typedef struct
{
    UWORD16 min_block_size;    /* Minimum block size in samples     */
    UWORD16 max_block_size;    /* Maximum block size in samples     */
    UWORD32  min_frame_size;   /* Minimum frame size (in bytes)     */
    UWORD32  max_frame_size;   /* Maximum frame size (in bytes)     */

    UWORD32  sample_rate;      /* Sample_rate in Hz (0 is invalid)  */
    UWORD8  num_channels;      /* Number of channels                */

    UWORD8  bits_per_sample;   /* Bits_for_sample                   */
    UWORD64 total_num_samples; /* Total Number of Samples available */

    WORD8   md5_sign[16];	   /* MD5 signature. It is of 16 bytes	*/
} flacp_stream_info_t;

/* This structure used to provide information about the stream info metadata */
/* block and vorbis comment block 											 */




typedef  struct 
{
		
		UWORD32 picture_type;
		
		UWORD32 mime_type_length;
		UWORD64 mime_type_offset;
		
		UWORD32 description_string_length;
		UWORD64 description_string_offset;
		
		UWORD32 picture_width;
		UWORD32 picture_height;
		UWORD32 picture_depth;
		UWORD32 number_of_colors_used;
		UWORD32 length_of_picture;
		UWORD64 picture_data_offset;
		
		
		UWORD8 has_picture; //0 if no 1 if true
		UWORD8 is_url; // 0 is no 1 if yes



}flacp_picture_info_t;
	//flacp_metadata_info_t		meta_data;

typedef struct
{
	UWORD32 number_of_comments;
	UWORD32 comment_length[MAX_VORBIS_COMMENT] ;
	UWORD64 offset_to_first_comment;
	UWORD8 has_comment;
	UWORD32 vorbis_size;


}flacp_vorbis_comment_info_t;
	





typedef  struct 
{
		flacp_picture_info_t picture_data;
		flacp_vorbis_comment_info_t vorbis_comment;


}flacp_metadata_info_t;
	
	



typedef struct
{
    flacp_stream_info_t stream_info;    /* Stream info metadata block        */
    UWORD32				comment_size;   /* Vorbis Comment block size		 */
    void				*comment_block; /* Vorbis Comment block data		 */

	flacp_metadata_info_t metadata;


} flacp_file_info_t;

/* This structure used by system to provide target sample number to seek to  */
/* and buffer which will be used to read audio data while seeking. Parser	 */
/* expects temp buffer of 1024 bytes to seek to the targer sample number.    */
typedef struct
{
	UWORD64 sample_num; /* Target sample number			   */
	void    *seek_buf;  /* Scratch buffer used during seek */
} flacp_seek_params_t;

/*****************************************************************************/
/* Extern Function Declarations                                              */
/*****************************************************************************/

/* Initializes the FLAC parser with the initialization attributes given by   */
/* the ' system/application '                                           	 */
extern WORD32 flacp_init(IN  flacp_t          *flacp_handle,
                         IN  flacp_attr_t     *attr,
                         IN  flacp_cb_funcs_t *cb_funcs);

/* Parses Stream information and counts number of application and picture    */
/* metadata blocks present in the given input file and provides to system	 */
extern WORD32 flacp_get_file_info(IN  flacp_t	        *flacp_handle,
							  	  OUT flacp_file_info_t	*file_info);

/* Frees the memory allocated to comment block data, if allocated in         */
/* flacp_get_file_info API                                               	 */
extern WORD32 flacp_free_file_info(IN flacp_t	        *flacp_handle,
							  	   IN flacp_file_info_t	*file_info);

/* Reads audio data of requested size if possible and provides to the system */
/* If requested amount of data is not available, then Parser reads amount of */
/* data available in the input file											 */
extern WORD32 flacp_read_audio_data(IN    flacp_t *flacp_handle,
                                    INOUT UWORD32 *bytes_to_be_read,
                                    OUT   void    *data_buf);

/* Seeks to the sample number requested by system. Parser uses binary search.*/
/* Updates file pointer to the start of the frame, which contains the target */
/* sample number requested by system. System should provide a buffer of 1024 */
/* bytes to seek to the frame which contains requested sample number. Parser */
/* cannot seek to the target sample number. It can only seek to the frame	 */
/* start which contains requested sample number.							 */
extern WORD32 flacp_seek(IN flacp_t			    *flacp_handle,
						 IN flacp_seek_params_t *seek_attr);

/* Releases the resources used by the FLAC parser instance					 */
extern WORD32 flacp_close(IN flacp_t *flacp_handle);

/* Used for getting library version											 */
extern WORD8 *flacp_get_version(void);

/*****************************************************************************/
/* C linkage specifiers for C++ declarations                                 */
/*****************************************************************************/

#ifdef __cplusplus
}                       /* End of extern "C" { */
#endif  /* __cplusplus */

#endif /* FLACP_H */
